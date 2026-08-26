# The challenge simulator ships as an x86-64 ELF binary only, so this image is
# pinned to linux/amd64. On Apple Silicon it runs under Rosetta; on Windows it
# runs natively through Docker Desktop's WSL2 backend.
FROM --platform=linux/amd64 ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        git \
        file \
        ca-certificates \
        python3 \
        python3-venv \
        python3-pip \
    && rm -rf /var/lib/apt/lists/*

# The venv lives outside /work so the bind mount cannot shadow it. Putting it
# first on PATH makes bare `python` and `pip` the venv's, with no activation,
# and sidesteps Ubuntu 24.04's PEP 668 block on pip installs.
RUN python3 -m venv /opt/venv
ENV PATH="/opt/venv/bin:${PATH}"
ENV VIRTUAL_ENV=/opt/venv

# Dependencies before any other COPY so this layer caches across source edits.
COPY requirements.txt /tmp/requirements.txt
RUN pip install --no-cache-dir -r /tmp/requirements.txt

WORKDIR /work

CMD ["bash"]
