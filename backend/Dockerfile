FROM python:3.12-slim

ENV PYTHONDONTWRITEBYTECODE=1
ENV PYTHONUNBUFFERED=1

WORKDIR /app

ARG TARGETARCH=amd64
ARG PIPER_VERSION=2023.11.14-2
ARG PIPER_VOICE_URL=https://huggingface.co/rhasspy/piper-voices/resolve/v1.0.0/es/es_AR/daniela/high/es_AR-daniela-high

RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates curl ffmpeg libavcodec-extra \
    && rm -rf /var/lib/apt/lists/* \
    && case "${TARGETARCH}" in \
         amd64) piper_arch=x86_64 ;; \
         arm64) piper_arch=aarch64 ;; \
         *) echo "unsupported TARGETARCH=${TARGETARCH}" >&2; exit 1 ;; \
       esac \
    && curl -fsSL "https://github.com/rhasspy/piper/releases/download/${PIPER_VERSION}/piper_linux_${piper_arch}.tar.gz" \
       | tar -xz -C /opt \
    && mkdir -p /opt/piper-voices \
    && curl -fsSL -o /opt/piper-voices/es_AR-daniela-high.onnx "${PIPER_VOICE_URL}.onnx" \
    && curl -fsSL -o /opt/piper-voices/es_AR-daniela-high.onnx.json "${PIPER_VOICE_URL}.onnx.json" \
    && test "$(stat -c%s /opt/piper-voices/es_AR-daniela-high.onnx)" -gt 10000000

ENV PIPER_BIN=/opt/piper/piper
ENV PIPER_MODEL=/opt/piper-voices/es_AR-daniela-high.onnx

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY app ./app

EXPOSE 8000

CMD ["sh", "-c", "python -m app.init_db && gunicorn --bind 0.0.0.0:8000 --workers 2 --timeout 90 app.main:app"]
