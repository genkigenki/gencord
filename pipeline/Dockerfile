# CUDA 12 + cuDNN 9: what faster-whisper / CTranslate2 4.x expect. Turing (RTX 20xx) is supported.
FROM nvidia/cuda:12.4.1-cudnn-runtime-ubuntu22.04

ENV DEBIAN_FRONTEND=noninteractive PYTHONUNBUFFERED=1 HF_HOME=/models
RUN apt-get update \
 && apt-get install -y --no-install-recommends python3 python3-pip ffmpeg ca-certificates \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY requirements.txt .
RUN pip3 install --no-cache-dir -r requirements.txt
COPY gencord_pipeline.py .

CMD ["python3", "gencord_pipeline.py"]
