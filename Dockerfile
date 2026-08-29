ARG VITASDK_IMAGE=gnuton/vitasdk-docker@sha256:62596beb9cf177226dafaac1b06b82a38df35f8424588b68a8ae395558e4a5fb

FROM ${VITASDK_IMAGE} AS vitasdk

FROM ubuntu:24.04

RUN apt-get update \
	&& DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
		ca-certificates \
		cmake \
		ninja-build \
	&& rm -rf /var/lib/apt/lists/*

COPY --from=vitasdk /usr/local/vitasdk /usr/local/vitasdk

ENV VITASDK=/usr/local/vitasdk
ENV PATH=/usr/local/vitasdk/bin:${PATH}

WORKDIR /work
