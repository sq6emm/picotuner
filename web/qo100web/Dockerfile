FROM python:3.12-slim
RUN apt-get update && apt-get install -y --no-install-recommends ffmpeg && rm -rf /var/lib/apt/lists/*
RUN pip install --no-cache-dir aiohttp==3.10.10
WORKDIR /app
COPY app.py /app/
COPY static /app/static
USER 1000:1000
CMD ["python", "-u", "/app/app.py"]
