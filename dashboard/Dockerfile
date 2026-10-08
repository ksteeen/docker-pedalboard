FROM python:3.11-alpine
WORKDIR /app

RUN apk add --no-cache gcc musl-dev linux-headers
RUN pip install --no-cache-dir fastapi uvicorn docker jinja2 pydantic

COPY dashboard/main.py .
COPY dashboard/templates ./templates

CMD ["uvicorn", "main:app", "--host", "0.0.0.0", "--port", "80"]
