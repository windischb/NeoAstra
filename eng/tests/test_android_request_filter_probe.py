from __future__ import annotations

import http.client
from pathlib import Path
import socket
import ssl
import struct
import tempfile
import unittest
import xml.etree.ElementTree as ET

from eng import run_android_request_filter_probe as probe


class AndroidRequestFilterProbeTests(unittest.TestCase):
    @staticmethod
    def controls() -> list[dict]:
        return [{"role": role, "bytes": 100, "headers_complete": True, "query": {"case": case, "v": view}}
                for role, case, view, expectation in probe.EXPECTATIONS if expectation == "some"]

    def test_every_allowed_control_is_required(self) -> None:
        records = self.controls()
        self.assertEqual([], probe.verify_host_counters(records))
        for index in range(len(records)):
            self.assertTrue(probe.verify_host_counters(records[:index] + records[index + 1:]))

    def test_every_refused_case_rejects_even_one_byte(self) -> None:
        for role, case, view, expectation in probe.EXPECTATIONS:
            if expectation == "none":
                leak = {"role": role, "bytes": 1, "headers_complete": False, "query": {"case": case, "v": view}}
                self.assertTrue(probe.verify_host_counters(self.controls() + [leak]))

    def test_unattributed_partial_request_cannot_pass(self) -> None:
        self.assertTrue(probe.verify_host_counters(self.controls() + [
            {"role": "target", "bytes": 4, "headers_complete": False, "request_line": "GET "}
        ]))

    def test_empty_transport_connection_is_only_diagnostic(self) -> None:
        self.assertEqual([], probe.verify_host_counters(self.controls() + [
            {"role": "target", "bytes": 0, "headers_complete": False}
        ]))

    def test_server_records_a_complete_control(self) -> None:
        servers = probe.HostServers()
        try:
            host, port = servers.sockets["source"].getsockname()
            client = http.client.HTTPConnection(host, port, timeout=3)
            client.request("GET", "/page?v=A&case=allowedSource")
            response = client.getresponse()
            self.assertEqual(200, response.status)
            self.assertIn(b"<html>", response.read())
            client.close()
        finally:
            records = servers.close()
        self.assertEqual(1, len(records))
        self.assertTrue(records[0]["headers_complete"])

    def test_close_drains_a_partial_request_before_returning_records(self) -> None:
        servers = probe.HostServers()
        with socket.create_connection(servers.sockets["target"].getsockname(), timeout=3) as connection:
            partial = b"GET /item?v=A&case=fetch HTTP/1.1\r\n"
            connection.sendall(partial)
            records = servers.close()
        self.assertEqual(1, len(records))
        self.assertEqual(len(partial), records[0]["bytes"])
        self.assertFalse(records[0]["headers_complete"])

    def test_frame_parameters_cannot_end_the_script(self) -> None:
        html = probe.frame_page({"target": "</script><script>bad()", "v": "A", "case": "frameSame"})
        self.assertEqual(1, html.count("</script>"))
        self.assertIn("\\u003c/script\\u003e", html)

    def test_tls_fixture_requires_its_ca_and_checks_hostname(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            ca, certificate, key = probe.create_tls_fixture(Path(temporary))
            server_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            server_context.load_cert_chain(certificate, key)
            servers = probe.HostServers(server_context)
            try:
                host, port = servers.sockets["source"].getsockname()
                self.assertEqual(f"https://127.0.0.1:{port}", servers.origin("source"))
                untrusted = http.client.HTTPSConnection(host, port, timeout=3)
                try:
                    with self.assertRaises(ssl.SSLCertVerificationError):
                        untrusted.request("GET", "/page")
                finally:
                    untrusted.close()
                trusted = ssl.create_default_context(cafile=ca)
                with socket.create_connection((host, port), timeout=3) as connection:
                    with self.assertRaises(ssl.SSLCertVerificationError):
                        trusted.wrap_socket(connection, server_hostname="wrong.example.test")
                client = http.client.HTTPSConnection(host, port, timeout=3, context=trusted)
                try:
                    client.request("GET", "/page?v=B&case=initial")
                    response = client.getresponse()
                    self.assertEqual(200, response.status)
                    self.assertIn(b"neoastra-probe-ready", response.read())
                finally:
                    client.close()
            finally:
                records = servers.close()
            requests = [record for record in records if record["bytes"]]
            self.assertEqual(1, len(requests))
            self.assertTrue(requests[0]["tls_established"])
            self.assertTrue(requests[0]["headers_complete"])
            self.assertEqual(2, sum(bool(record.get("tls_error")) for record in records))

    def test_test_ca_is_scoped_to_loopback_in_generated_resources(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            ca = root / "ca.pem"
            ca.write_text("test certificate")
            resources = root / "res"
            probe.write_probe_network_config(resources, ca)
            config = ET.parse(resources / "xml/probe_network_security.xml").getroot()
            self.assertEqual("false", config.find("base-config").get("cleartextTrafficPermitted"))
            domain = config.find("domain-config")
            self.assertEqual(["127.0.0.1"], [item.text for item in domain.findall("domain")])
            self.assertEqual("@raw/probe_ca", domain.find("trust-anchors/certificates").get("src"))
            self.assertEqual("test certificate", (resources / "raw/probe_ca.pem").read_text())

    def test_additional_resource_controls_have_usable_responses(self) -> None:
        servers = probe.HostServers()
        try:
            for path, expected in (("/style", "text/css"), ("/event", "text/event-stream"), ("/audio", "audio/wav")):
                status, mime, body = servers.respond("target", path, {})
                self.assertEqual(200, status)
                self.assertEqual(expected, mime)
                self.assertTrue(body)
            audio = servers.respond("target", "/audio", {})[2]
            self.assertEqual(b"RIFF", audio[:4])
            self.assertEqual(len(audio) - 8, struct.unpack("<I", audio[4:8])[0])
            self.assertIn(b"data: ready\n\n", servers.respond("target", "/event", {})[2])
        finally:
            servers.close()


if __name__ == "__main__":
    unittest.main()
