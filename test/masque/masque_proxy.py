#!/usr/bin/env python3
"""Minimal MASQUE CONNECT-UDP (RFC 9298) proxy for testing ttsignal's client.

It speaks HTTP/3 (ALPN "h3") to the ttsignal MASQUE tunnel, accepts an
Extended CONNECT request with :protocol=connect-udp targeting
/.well-known/masque/udp/{host}/{port}/, opens a real UDP socket to that
target, and relays packets both ways as HTTP/3 datagrams (RFC 9297):

    QUIC DATAGRAM payload = [Quarter-Stream-ID][Context-ID=0][UDP payload]

aioquic handles the Quarter-Stream-ID layer; this proxy only deals with the
Context-ID (always 0 for the base UDP flow) and the inner UDP payload.

Usage:
    masque_proxy.py [listen_host] [listen_port] [cert] [key]
    defaults: 127.0.0.1 4435 certs/localhost.crt certs/localhost.key
"""
import asyncio
import logging
import os
import sys
from urllib.parse import unquote

from aioquic.asyncio import serve
from aioquic.asyncio.protocol import QuicConnectionProtocol
from aioquic.h3.connection import H3Connection
from aioquic.h3.events import DatagramReceived, HeadersReceived
from aioquic.quic.configuration import QuicConfiguration

logger = logging.getLogger("masque-proxy")


def decode_varint(data: bytes, off: int):
    b0 = data[off]
    length = 1 << (b0 >> 6)
    val = b0 & 0x3F
    for i in range(1, length):
        val = (val << 8) | data[off + i]
    return val, off + length


def encode_varint(v: int) -> bytes:
    if v < 0x40:
        return bytes([v])
    if v < 0x4000:
        return bytes([0x40 | (v >> 8), v & 0xFF])
    if v < 0x40000000:
        return bytes([0x80 | (v >> 24), (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF])
    return bytes(
        [0xC0 | (v >> 56) & 0xFF]
        + [(v >> (8 * i)) & 0xFF for i in range(6, -1, -1)]
    )


def parse_connect_udp_path(path: str):
    """/.well-known/masque/udp/{target_host}/{target_port}/ -> (host, port)."""
    parts = [p for p in path.split("/") if p != ""]
    # expect: .well-known masque udp <host> <port>
    if len(parts) < 5 or parts[0] != ".well-known" or parts[1] != "masque" or parts[2] != "udp":
        return None
    host = unquote(parts[3])
    try:
        port = int(parts[4])
    except ValueError:
        return None
    return host, port


class _UdpRelay(asyncio.DatagramProtocol):
    def __init__(self, on_packet):
        self._on_packet = on_packet
        self.transport = None

    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, addr):
        self._on_packet(data)


class MasqueProxyProtocol(QuicConnectionProtocol):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._http = None
        self._udp = {}  # stream_id -> asyncio transport to target

    def quic_event_received(self, event):
        if self._http is None:
            self._http = H3Connection(self._quic, enable_webtransport=True)
        for h3_event in self._http.handle_event(event):
            self._h3_event(h3_event)

    def _h3_event(self, event):
        if isinstance(event, HeadersReceived):
            headers = {k: v for (k, v) in event.headers}
            method = headers.get(b":method", b"")
            protocol = headers.get(b":protocol", b"")
            path = headers.get(b":path", b"").decode("utf-8", "replace")
            if method == b"CONNECT" and protocol == b"connect-udp":
                logger.info("CONNECT-UDP request stream=%d path=%s", event.stream_id, path)
                asyncio.ensure_future(self._open_target(event.stream_id, path))
            else:
                logger.warning("rejecting non connect-udp request: %s %s", method, protocol)
                self._http.send_headers(event.stream_id, [(b":status", b"400")], end_stream=True)
                self.transmit()
        elif isinstance(event, DatagramReceived):
            self._client_to_target(event.stream_id, event.data)

    async def _open_target(self, stream_id, path):
        target = parse_connect_udp_path(path)
        if target is None:
            logger.warning("bad connect-udp path: %s", path)
            self._http.send_headers(stream_id, [(b":status", b"404")], end_stream=True)
            self.transmit()
            return
        host, port = target
        loop = asyncio.get_event_loop()

        def on_target_packet(udp: bytes):
            # target -> client: wrap as [Context-ID=0][udp] (aioquic adds QSID)
            logger.info("target->client stream=%d %d bytes", stream_id, len(udp))
            self._http.send_datagram(stream_id, encode_varint(0) + udp)
            self.transmit()

        try:
            transport, _ = await loop.create_datagram_endpoint(
                lambda: _UdpRelay(on_target_packet), remote_addr=(host, port)
            )
        except Exception as exc:  # noqa: BLE001
            logger.error("failed to open udp to %s:%d: %s", host, port, exc)
            self._http.send_headers(stream_id, [(b":status", b"502")], end_stream=True)
            self.transmit()
            return

        self._udp[stream_id] = transport
        logger.info("tunnel established stream=%d -> %s:%d", stream_id, host, port)
        self._http.send_headers(
            stream_id,
            [(b":status", b"200"), (b"capsule-protocol", b"?1")],
            end_stream=False,
        )
        self.transmit()

    def _client_to_target(self, stream_id, data: bytes):
        # client -> target: strip [Context-ID][...]; context 0 == UDP payload
        if not data:
            return
        ctx, off = decode_varint(data, 0)
        if ctx != 0:
            logger.debug("ignoring datagram with context-id=%d", ctx)
            return
        transport = self._udp.get(stream_id)
        if transport is not None:
            logger.info("client->target stream=%d %d bytes", stream_id, len(data) - off)
            transport.sendto(data[off:])


async def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 4435
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.abspath(os.path.join(here, "..", ".."))
    cert = sys.argv[3] if len(sys.argv) > 3 else os.path.join(repo, "certs", "localhost.crt")
    key = sys.argv[4] if len(sys.argv) > 4 else os.path.join(repo, "certs", "localhost.key")

    config = QuicConfiguration(
        is_client=False,
        alpn_protocols=["h3"],
        # NOTE: ttsignal/xquic stores the peer's max_datagram_frame_size in a
        # uint16_t, so 65536 (the aioquic default) overflows to 0 and silently
        # disables HTTP/3 datagrams. Advertise 65535 so it fits.
        max_datagram_frame_size=65535,
        # The inner QUIC client pads its Initial to 1200B; wrapped as an HTTP/3
        # datagram (~1216B) it must fit in ONE outer QUIC packet to the client.
        # aioquic's default outgoing packet size is 1200B, too small, so the
        # return datagram would stall forever at the head of the send queue.
        # Bump the outer packet size so the padded inner Initial can be relayed.
        max_datagram_size=1400,
    )
    config.load_cert_chain(cert, key)

    logger.info("MASQUE CONNECT-UDP proxy listening on %s:%d (h3)", host, port)
    await serve(host, port, configuration=config, create_protocol=MasqueProxyProtocol)
    await asyncio.Future()  # run forever


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s %(message)s")
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
