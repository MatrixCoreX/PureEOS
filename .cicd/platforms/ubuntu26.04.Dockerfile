# syntax=docker/dockerfile:1
FROM ubuntu:26.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build git pkg-config ca-certificates \
    libcurl4-openssl-dev libgmp-dev zlib1g-dev python3 python3-venv \
    python3-numpy file jq zstd

# The parallel new-chain profile uses eos-vm-jit. OC's LLVM7-11 dependency is
# intentionally not injected into a modern Ubuntu system.
ENV HELIOS_MODERN_JIT_PROFILE=1
