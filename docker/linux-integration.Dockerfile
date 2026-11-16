# SPDX-License-Identifier: GPL-3.0-or-later
# Builds unmodified upstream SOEM v2.0.1 for Linux and runs it against ecatsim over a
# veth pair. This validates the simulator against an independent, widely used master.
FROM ubuntu:24.04

ARG SOEM_URL=https://github.com/OpenEtherCATsociety/SOEM/archive/refs/tags/v2.0.1.tar.gz
ARG SOEM_SHA256=933208261f1737ae530d25b915c1ce459413dcdba13d92e95af3726380601648

RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential cmake ca-certificates curl iproute2 python3 python3-pytest \
 && rm -rf /var/lib/apt/lists/*

RUN curl -fsSL -o /tmp/soem.tar.gz "$SOEM_URL" \
 && echo "$SOEM_SHA256  /tmp/soem.tar.gz" | sha256sum -c - \
 && mkdir -p /opt/soem-src && tar -xzf /tmp/soem.tar.gz -C /opt/soem-src --strip-components=1 \
 && cmake -S /opt/soem-src -B /opt/soem-build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build /opt/soem-build -j"$(nproc)"

COPY sim /opt/etherq/sim
COPY scripts/linux-integration.sh /opt/etherq/scripts/linux-integration.sh
WORKDIR /opt/etherq
ENTRYPOINT ["/opt/etherq/scripts/linux-integration.sh"]
