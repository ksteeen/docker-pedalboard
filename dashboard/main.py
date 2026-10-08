from fastapi import FastAPI
from fastapi.responses import FileResponse
from pydantic import BaseModel
from typing import List
import docker
import json
import socket
import time

app = FastAPI()

CATEGORY_COLORS = {
    "distortion": "#9c27b0",
    "reverb": "#00bcd4",
    "delay": "#ff9800",
    "mod": "#4caf50"
}

class ParamUpdate(BaseModel):
    container_id: str
    param: str
    value: float

class ReorderRequest(BaseModel):
    order: List[str]

@app.get("/")
async def get_index():
    return FileResponse("templates/index.html")

@app.get("/api/pedals")
async def get_pedals():
    pedals = []
    try:
        client = docker.from_env()
        for container in client.containers.list():
            labels = container.labels
            if "pedal.name" in labels:
                category = labels.get("pedal.category", "default")
                order = int(labels.get("pedal.order", 99))
                try:
                    controls = json.loads(labels.get("pedal.controls", "[]"))
                except Exception:
                    controls = []

                # Зчитуємо реальний стан PEDAL_ACTIVE з середовища контейнера
                env_vars = container.attrs.get('Config', {}).get('Env', [])
                is_active = True
                for env in env_vars:
                    if env.startswith("PEDAL_ACTIVE="):
                        val = env.split("=")[1]
                        is_active = (val == "1")
                        break

                pedals.append({
                    "id": container.short_id,
                    "name": labels.get("pedal.name", "Unknown Pedal"),
                    "category": category,
                    "order": order,
                    "color": CATEGORY_COLORS.get(category, "#ff5722"),
                    "is_active": is_active,
                    "controls": controls
                })
        # Чітке стабільне сортування за фіксованим order
        pedals.sort(key=lambda x: x["order"])
    except Exception as e:
        print(f"Docker API Error: {e}")
    return pedals

@app.post("/api/param")
async def update_param(data: ParamUpdate):
    try:
        client = docker.from_env()
        container = client.containers.get(data.container_id)
        
        ip = None
        networks = container.attrs['NetworkSettings']['Networks']
        for net_name, net_data in networks.items():
            if 'pedal' in net_name:
                ip = net_data.get('IPAddress')
                break
        if not ip and networks:
            ip = next(iter(networks.values())).get('IPAddress')
        
        port = int(container.labels.get("pedal.control_port", 9000))
        
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        payload = f"{data.param}={data.value}".encode('utf-8')
        sock.sendto(payload, (ip, port))
        sock.close()
        return {"status": "ok"}
    except Exception as e:
        return {"status": "error", "message": str(e)}

@app.post("/api/reorder")
async def reorder_chain(data: ReorderRequest):
    try:
        client = docker.from_env()
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

        for idx, container_id in enumerate(data.order):
            in_slot = idx
            out_slot = idx + 1
            
            container = client.containers.get(container_id)
            networks = container.attrs['NetworkSettings']['Networks']
            ip = None
            for net_name, net_data in networks.items():
                if 'pedal' in net_name:
                    ip = net_data.get('IPAddress')
                    break
            if not ip and networks:
                ip = next(iter(networks.values())).get('IPAddress')
            
            port = int(container.labels.get("pedal.control_port", 9000))
            msg = f"route={in_slot}:{out_slot}".encode('utf-8')
            sock.sendto(msg, (ip, port))

        final_out_slot = len(data.order)
        engine_containers = client.containers.list(filters={"name": "engine"})
        if engine_containers:
            engine_ip = None
            net = engine_containers[0].attrs['NetworkSettings']['Networks']
            for net_name, net_data in net.items():
                if 'pedal' in net_name:
                    engine_ip = net_data.get('IPAddress')
                    break
            if not engine_ip and net:
                engine_ip = next(iter(net.values())).get('IPAddress')
            
            sock.sendto(f"out_slot={final_out_slot}".encode('utf-8'), (engine_ip, 9009))

        sock.close()
        return {"status": "ok", "final_slot": final_out_slot}
    except Exception as e:
        return {"status": "error", "message": str(e)}

if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=80)
