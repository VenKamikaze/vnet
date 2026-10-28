# syntax=docker/dockerfile:1
#
# The vnet relay as a small container: one static binary, nothing else.
#
#   docker build -t vnet-relay relay/
#   docker run --rm -p 7777:7777/udp -p 8080:8080 vnet-relay
#
# Native clients use UDP port 7777; browsers connect to ws://HOST:8080/vnet
# (put TLS in front for wss://, see relay/README.md). Flags and VNET_RELAY_*
# environment variables are described there too, e.g.
#   docker run ... -e VNET_RELAY_MAX_MEMBERS=4 vnet-relay

FROM golang:1.24-alpine AS build
WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
COPY . .
RUN CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o /out/vnet-relay ./cmd/vnet-relay

FROM scratch
COPY --from=build /out/vnet-relay /vnet-relay
EXPOSE 7777/udp 8080/tcp
USER 65534:65534
ENTRYPOINT ["/vnet-relay"]
