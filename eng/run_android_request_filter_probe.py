#!/usr/bin/env python3
"""Build and run the Android request-filter probe in disposable emulators.

The APK is built from the real backend sources (native/src/android/java), the probe activity and the native libraries of
the CMake target neoastra_android_request_filter_probe, with the installed SDK tools and the AndroidX libraries already in
the NuGet cache: nothing is downloaded. Each API level gets an AVD of its own in a temporary ANDROID_AVD_HOME and a
console port that no running emulator uses; adb is only ever called with that emulator's serial. A run passes only when
the activity wrote PASS to files/result.txt and the host servers confirm what reached them.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time
from urllib.parse import parse_qs, urlsplit
import xml.etree.ElementTree as ET
import zipfile


ROOT = Path(__file__).resolve().parents[1]
PACKAGE = "io.neoastra.filtertest"
ACTIVITY = f"{PACKAGE}/.ProbeActivity"
JAVA_DIR = ROOT / "native/src/android/java"
PROBE_DIR = ROOT / "native/tests/android_request_filter"
NATIVE_BUILD = ROOT / "artifacts/native/android-request-filter"
NATIVE_LIBRARIES = ("libneoastra_native.so", "libneoastra_android_request_filter_probe.so")
LOG_ROOT = ROOT / "artifacts/native/android-request-filter-probe"
# The AndroidX WebKit version the samples pin; its dependencies are resolved from the nuspec files of the NuGet cache.
ROOT_PACKAGES = (("Xamarin.AndroidX.WebKit", "1.17.1"),)
COMPILE_SDK = "36"
BUILD_TOOLS = "36.0.0"
NDK = "29.0.14206865"
EMULATOR_GATEWAY = "127.0.0.1"
ANDROID_NS = "http://schemas.android.com/apk/res/android"
ET.register_namespace("android", ANDROID_NS)


def log(message: str) -> None:
    print(message, flush=True)


def run(*command: str | Path, timeout: int = 600, env: dict[str, str] | None = None, check: bool = True) -> str:
    text = [str(part) for part in command]
    log("+ " + " ".join(text))
    result = subprocess.run(text, cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout,
                            env=env)
    if result.stdout:
        print(result.stdout, end="" if result.stdout.endswith("\n") else "\n", flush=True)
    if check:
        result.check_returncode()
    return result.stdout


# --- Dependencies from the NuGet cache -------------------------------------------------------------------------------

def version_key(version: str) -> tuple[int, ...]:
    return tuple(int(part) if part.isdigit() else 0 for part in re.split(r"[.\-]", version))


def resolve_packages(cache: Path) -> list[Path]:
    """Resolves the nuspec dependencies as NuGet does: each package gets the highest minimum any dependent asks for, which
    must be in the cache (else the next higher cached version); repeated until the closure no longer changes."""
    minimums = {package_id.lower(): version for package_id, version in ROOT_PACKAGES}
    while True:
        resolved: dict[str, Path] = {}
        missing: list[str] = []
        changed = False
        pending = list(minimums)
        while pending:
            key = pending.pop()
            if key in resolved:
                continue
            folder = cache / key
            versions = sorted((entry.name for entry in folder.iterdir() if entry.is_dir()), key=version_key) if folder.is_dir() else []
            candidates = [version for version in versions if version_key(version) >= version_key(minimums[key])]
            if not candidates:
                missing.append(f"{key} >= {minimums[key]} (cached: {', '.join(versions) or 'none'})")
                continue
            chosen = minimums[key] if minimums[key] in candidates else candidates[0]
            resolved[key] = folder / chosen
            nuspec = next((folder / chosen).glob("*.nuspec"))
            for match in re.finditer(r'<dependency id="([^"]+)" version="\[?([^",\]\)]+)', nuspec.read_text(encoding="utf-8")):
                if not match.group(1).startswith("Xamarin."):
                    continue
                dependency, version = match.group(1).lower(), match.group(2)
                if dependency not in minimums or version_key(version) > version_key(minimums[dependency]):
                    minimums[dependency] = version
                    changed = True
                pending.append(dependency)
        if missing:
            raise RuntimeError("missing in the NuGet cache (no downloads): " + "; ".join(missing))
        if not changed:
            return sorted(resolved.values())


def package_archives(package: Path) -> list[Path]:
    """The Java archives of one package: aar/ or jar/, else the library of the newest Android target framework."""
    for pattern in ("aar/*.aar", "jar/*.jar"):
        found = sorted(package.glob(pattern))
        if found:
            return found
    frameworks = sorted((entry for entry in (package / "lib").glob("net*-android*") if entry.is_dir()),
                        key=lambda entry: version_key(re.sub(r"[^0-9.]", "", entry.name.split("-android")[1])))
    if frameworks:
        return sorted([*frameworks[-1].glob("*.aar"), *frameworks[-1].glob("*.jar")])
    return []


class Library:
    def __init__(self, name: str) -> None:
        self.name = name
        self.jars: list[Path] = []
        self.res: Path | None = None
        self.manifest: Path | None = None
        self.package: str | None = None


def extract_libraries(packages: list[Path], work: Path) -> list[Library]:
    libraries: list[Library] = []
    for package in packages:
        for archive in package_archives(package):
            library = Library(f"{package.parent.name}-{package.name}-{archive.stem}")
            if archive.suffix == ".jar":
                library.jars.append(archive)
            else:
                target = work / "aar" / library.name
                with zipfile.ZipFile(archive) as aar:
                    aar.extractall(target)
                library.jars = sorted([*target.glob("classes.jar"), *target.glob("libs/*.jar")])
                if (target / "res").is_dir() and any((target / "res").rglob("*.*")):
                    library.res = target / "res"
                if (target / "AndroidManifest.xml").is_file():
                    library.manifest = target / "AndroidManifest.xml"
                    library.package = ET.parse(library.manifest).getroot().get("package")
            if library.jars or library.res:
                libraries.append(library)
    return libraries


# --- Manifest merge --------------------------------------------------------------------------------------------------

def merge_manifests(main: Path, overlays: list[Path], output: Path) -> None:
    """Adds the top-level declarations and the application components of the overlays to the probe manifest, as the
    manifest merger of a .NET Android build does for these simple overlays; ${applicationId} becomes the package."""
    tree = ET.parse(main)
    root = tree.getroot()
    application = root.find("application")
    name_attribute = f"{{{ANDROID_NS}}}name"

    def key(element: ET.Element) -> tuple[str, str | None]:
        return element.tag, element.get(name_attribute)

    top = {key(element) for element in root}
    components = {key(element) for element in application}
    for overlay in overlays:
        text = overlay.read_text(encoding="utf-8").replace("${applicationId}", PACKAGE)
        overlay_root = ET.fromstring(text)
        for element in overlay_root:
            if element.tag == "application":
                for attribute, value in element.attrib.items():
                    application.attrib.setdefault(attribute, value)
                for component in element:
                    if key(component) not in components:
                        application.append(component)
                        components.add(key(component))
            elif element.tag != "uses-sdk" and key(element) not in top:
                root.insert(list(root).index(application), element)
                top.add(key(element))
    tree.write(output, encoding="utf-8", xml_declaration=True)


# --- APK build -------------------------------------------------------------------------------------------------------

def build_native(env: dict[str, str]) -> None:
    run("cmake", "--preset", "android-arm64-release", "-B", NATIVE_BUILD, "-DNEOASTRA_BUILD_ANDROID_REQUEST_FILTER_PROBE=ON",
        env=env)
    run("cmake", "--build", NATIVE_BUILD, "--target", "neoastra_android_request_filter_probe", env=env)


def find_native_libraries() -> list[Path]:
    found = []
    for name in NATIVE_LIBRARIES:
        matches = sorted(NATIVE_BUILD.rglob(name))
        if not matches:
            raise RuntimeError(f"{name} not found under {NATIVE_BUILD}: build the target neoastra_android_request_filter_probe")
        found.append(matches[0])
    return found


def create_tls_fixture(work: Path) -> tuple[Path, Path, Path]:
    """Create a disposable CA and loopback server certificate; never install trust on the host/device."""
    work.mkdir(parents=True, exist_ok=True)
    ca, ca_key = work / "ca.pem", work / "ca.key"
    certificate, key, request = work / "server.pem", work / "server.key", work / "server.csr"
    config = work / "certificate.cnf"
    config.write_text(
        "[req]\nprompt=no\ndistinguished_name=dn\n[dn]\nCN=NeoAstra disposable probe\n"
        "[ca]\nbasicConstraints=critical,CA:true\nkeyUsage=critical,keyCertSign,cRLSign\n"
        "subjectKeyIdentifier=hash\n"
        "[server]\nbasicConstraints=critical,CA:false\nkeyUsage=critical,digitalSignature,keyEncipherment\n"
        "extendedKeyUsage=serverAuth\nauthorityKeyIdentifier=keyid,issuer\n"
        "subjectAltName=IP:127.0.0.1\n", encoding="utf-8")
    run("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2", "-sha256",
        "-config", config, "-extensions", "ca", "-keyout", ca_key, "-out", ca)
    run("openssl", "req", "-newkey", "rsa:2048", "-nodes", "-config", config,
        "-subj", "/CN=127.0.0.1", "-keyout", key, "-out", request)
    run("openssl", "x509", "-req", "-in", request, "-CA", ca, "-CAkey", ca_key, "-CAcreateserial",
        "-days", "2", "-sha256", "-extfile", config, "-extensions", "server", "-out", certificate)
    return ca, certificate, key


def write_probe_network_config(resources: Path, ca: Path) -> None:
    """Only the disposable probe APK trusts this CA, and only for its loopback fixture."""
    (resources / "raw").mkdir(parents=True, exist_ok=True)
    (resources / "xml").mkdir(parents=True, exist_ok=True)
    shutil.copy(ca, resources / "raw/probe_ca.pem")
    (resources / "xml/probe_network_security.xml").write_text(
        '<network-security-config><base-config cleartextTrafficPermitted="false"/>'
        '<domain-config cleartextTrafficPermitted="false"><domain>127.0.0.1</domain>'
        '<trust-anchors><certificates src="@raw/probe_ca"/></trust-anchors>'
        '</domain-config></network-security-config>', encoding="utf-8")


def build_apk(sdk: Path, work: Path, java_only: bool, tls_ca: Path | None = None) -> Path:
    tools = sdk / "build-tools" / BUILD_TOOLS
    android_jar = sdk / "platforms" / f"android-{COMPILE_SDK}" / "android.jar"
    for required in (tools / "aapt2", tools / "d8", tools / "zipalign", tools / "apksigner", android_jar):
        if not required.exists():
            raise RuntimeError(f"missing SDK tool or platform (no installation): {required}")
    packages = resolve_packages(Path(os.environ.get("NUGET_PACKAGES", Path.home() / ".nuget/packages")))
    (work / "packages.txt").write_text("\n".join(str(package) for package in packages) + "\n", encoding="utf-8")
    log("AndroidX/Kotlin packages:\n  " + "\n  ".join(f"{package.parent.name} {package.name}" for package in packages))
    libraries = extract_libraries(packages, work)
    jars = [jar for library in libraries for jar in library.jars]

    # Resources: those of the libraries, then the one of the backend that its manifest names (as NeoAstra.Core.targets).
    compiled = work / "compiled"
    compiled.mkdir()
    overlays: list[Path] = []
    for library in libraries:
        if library.res is not None:
            target = compiled / f"{library.name}.zip"
            run(tools / "aapt2", "compile", "--dir", library.res, "-o", target)
            overlays.append(target)
    backend_res = work / "backend-res"
    (backend_res / "xml").mkdir(parents=True)
    shutil.copy(JAVA_DIR / "xml/neoastra_file_paths.xml", backend_res / "xml")
    if tls_ca is not None:
        write_probe_network_config(backend_res, tls_ca)
    backend_zip = compiled / "backend.zip"
    run(tools / "aapt2", "compile", "--dir", backend_res, "-o", backend_zip)

    manifest = work / "AndroidManifest.xml"
    merge_manifests(PROBE_DIR / "AndroidManifest.xml",
                    [JAVA_DIR / "AndroidManifest.xml", *(library.manifest for library in libraries if library.manifest)],
                    manifest)
    if tls_ca is not None:
        tree = ET.parse(manifest)
        tree.getroot().find("application").set(f"{{{ANDROID_NS}}}networkSecurityConfig", "@xml/probe_network_security")
        tree.write(manifest, encoding="utf-8", xml_declaration=True)
    generated = work / "generated"
    resources_apk = work / "resources.apk"
    extra_packages = sorted({library.package for library in libraries if library.package and library.res is not None})
    link = [tools / "aapt2", "link", "-I", android_jar, "--manifest", manifest, "--java", generated, "--debug-mode",
            "--auto-add-overlay", "-o", resources_apk, backend_zip]
    for overlay in overlays:
        link += ["-R", overlay]
    if extra_packages:
        link += ["--extra-packages", ":".join(extra_packages)]
    run(*link)

    # Java: every backend source, the probe sources and the generated R classes.
    sources = sorted([*(JAVA_DIR / "com/neoastra/backend").glob("*.java"), *PROBE_DIR.rglob("*.java"), *generated.rglob("*.java")])
    if not java_only and not any(source.is_relative_to(PROBE_DIR) for source in sources):
        raise RuntimeError(f"no probe activity under {PROBE_DIR}")
    classes = work / "classes"
    classpath = os.pathsep.join(str(path) for path in [android_jar, *jars])
    run("javac", "--release", "11", "-encoding", "UTF-8", "-Xlint:-options", "-cp", classpath, "-d", classes, *sources)
    if java_only:
        return resources_apk
    dex = work / "dex"
    dex.mkdir()
    run(tools / "d8", "--debug", "--min-api", "24", "--lib", android_jar, "--output", dex,
        *sorted(classes.rglob("*.class")), *jars, timeout=1200)

    unsigned = work / "unsigned.apk"
    shutil.copy(resources_apk, unsigned)
    with zipfile.ZipFile(unsigned, "a", zipfile.ZIP_DEFLATED) as apk:
        for dex_file in sorted(dex.glob("classes*.dex")):
            apk.write(dex_file, dex_file.name)
        for library in find_native_libraries():
            apk.write(library, f"lib/arm64-v8a/{library.name}")
    aligned = work / "aligned.apk"
    run(tools / "zipalign", "-f", "-p", "4", unsigned, aligned)
    keystore = work / "probe.keystore"
    run("keytool", "-genkeypair", "-keystore", keystore, "-storepass", "probepass", "-keypass", "probepass", "-alias", "probe",
        "-keyalg", "RSA", "-keysize", "2048", "-validity", "2", "-dname", "CN=neoastra-filtertest")
    apk = work / "probe.apk"
    run(tools / "apksigner", "sign", "--ks", keystore, "--ks-pass", "pass:probepass", "--out", apk, aligned)
    run(tools / "apksigner", "verify", apk)
    return apk


# --- Host servers ----------------------------------------------------------------------------------------------------

GIF = bytes.fromhex("47494638396101000100800000000000ffffff21f90401000000002c00000000010001000002024401003b")
# 100 ms of valid 8-bit mono PCM silence; no external media fixture/download.
AUDIO = (b"RIFF" + struct.pack("<I", 836) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 1, 8000, 8000, 1, 8)
         + b"data" + struct.pack("<I", 800) + bytes([128]) * 800)


def frame_page(query: dict[str, str]) -> str:
    """A frame that fetches and loads an image from the target, waits for both results, then reports to its parent.
    The parameters enter the script only as JSON, with the characters that could end the script element escaped."""
    def literal(value: str) -> str:
        return json.dumps(value).replace("<", "\\u003c").replace(">", "\\u003e").replace("&", "\\u0026") \
            .replace("\u2028", "\\u2028").replace("\u2029", "\\u2029")
    target, view, case = (literal(query.get(name, "")) for name in ("target", "v", "case"))
    return f"""<!doctype html><html><head><meta charset="utf-8"></head><body><script>
const target={target},v={view},name={case};
const url=(path,suffix)=>target+path+'?v='+encodeURIComponent(v)+'&case='+encodeURIComponent(name+suffix);
const fetchStatus=fetch(url('/item','Fetch'),{{cache:'no-store'}}).then(r=>r.status,()=>0);
const imageResult=new Promise(resolve=>{{const i=new Image();i.onload=()=>resolve('load');i.onerror=()=>resolve('error');i.src=url('/image','Image');}});
Promise.all([fetchStatus,imageResult]).then(([s,i])=>parent.postMessage({{probeFrame:name,fetchStatus:s,imageResult:i}},'*'));
</script></body></html>"""


class HostServers:
    """Raw HTTP servers on 127.0.0.1 for source, cross and target, reached through per-emulator adb reverse mappings.
    Every accepted connection is recorded with its bytes, so that a connection without a request stays visible."""

    ROLES = ("source", "cross", "target")

    def __init__(self, tls_context: ssl.SSLContext | None = None) -> None:
        self.tls_context = tls_context
        self.records: list[dict] = []
        self.lock = threading.Lock()
        self.sockets: dict[str, socket.socket] = {}
        self.stopping = threading.Event()
        self.acceptors: list[threading.Thread] = []
        self.workers: list[threading.Thread] = []
        self.started = time.monotonic()
        for role in self.ROLES:
            listener = socket.socket()
            listener.bind(("127.0.0.1", 0))
            listener.listen(64)
            listener.settimeout(0.1)
            self.sockets[role] = listener
            acceptor = threading.Thread(target=self.serve, args=(role, listener), daemon=True)
            self.acceptors.append(acceptor)
            acceptor.start()

    def origin(self, role: str) -> str:
        scheme = "https" if self.tls_context is not None else "http"
        return f"{scheme}://{EMULATOR_GATEWAY}:{self.sockets[role].getsockname()[1]}"

    def serve(self, role: str, listener: socket.socket) -> None:
        while True:
            try:
                connection, _ = listener.accept()
            except socket.timeout:
                if self.stopping.is_set():
                    return
                continue
            except OSError:
                return
            worker = threading.Thread(target=self.handle, args=(role, connection), daemon=True)
            self.workers.append(worker)  # Read only after all acceptors have joined.
            worker.start()

    def handle(self, role: str, connection: socket.socket) -> None:
        data = b""
        deadline = time.monotonic() + 2
        tls_established = False
        tls_error = None
        try:
            if self.tls_context is not None:
                connection.settimeout(2)
                connection = self.tls_context.wrap_socket(connection, server_side=True)
                tls_established = True
            while b"\r\n\r\n" not in data and len(data) < 65536 and time.monotonic() < deadline:
                connection.settimeout(max(0.01, deadline - time.monotonic()))
                chunk = connection.recv(65536 - len(data))
                if not chunk:
                    break
                data += chunk
        except OSError as error:
            if self.tls_context is not None and not tls_established:
                tls_error = type(error).__name__
        record: dict = {"t": round(time.monotonic() - self.started, 3), "role": role, "bytes": len(data),
                        "headers_complete": b"\r\n\r\n" in data, "tls_established": tls_established}
        if tls_error:
            record["tls_error"] = tls_error
        if data:
            line = data.split(b"\r\n", 1)[0].decode("latin-1")
            record["request_line"] = line
            parts = line.split(" ")
            if len(parts) >= 2:
                target = urlsplit(parts[1])
                record["path"] = target.path
                record["query"] = {name: values[0] for name, values in parse_qs(target.query).items()}
                status, content_type, body = self.respond(role, target.path, record["query"])
                head = (f"HTTP/1.1 {status} X\r\nContent-Type: {content_type}\r\nContent-Length: {len(body)}\r\n"
                        "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n")
                try:
                    connection.sendall(head.encode() + body)
                except OSError:
                    pass
        with self.lock:
            self.records.append(record)
        connection.close()

    def respond(self, role: str, path: str, query: dict[str, str]) -> tuple[int, str, bytes]:
        """The endpoints of ProbeActivity; every origin serves all of them."""
        if path == "/page":
            return 200, "text/html; charset=utf-8", b"<!doctype html><html><head><meta charset=\"utf-8\"></head><body>neoastra-probe-ready</body></html>"
        if path == "/item":
            return 200, "text/plain; charset=utf-8", b"ok"
        if path == "/image":
            return 200, "image/gif", GIF
        if path == "/script":
            return 200, "application/javascript", b"window.__probeScript=(window.__probeScript||0)+1;"
        if path == "/style":
            return 200, "text/css", b"body { --neoastra-probe: ready; }"
        if path == "/event":
            return 200, "text/event-stream", b"data: ready\n\n"
        if path == "/audio":
            return 200, "audio/wav", AUDIO
        if path == "/frame":
            return 200, "text/html; charset=utf-8", frame_page(query).encode()
        return 404, "text/plain; charset=utf-8", b"not found"

    def close(self) -> list[dict]:
        # Drain pending accepts and every handler before taking the snapshot.
        # A sleep or only complete HTTP headers could hide partial request bytes.
        self.stopping.set()
        for acceptor in self.acceptors:
            acceptor.join(timeout=3)
        for listener in self.sockets.values():
            listener.close()
        for worker in self.workers:
            worker.join(timeout=3)
        if any(thread.is_alive() for thread in [*self.acceptors, *self.workers]):
            raise RuntimeError("host observation did not finish draining")
        with self.lock:
            return list(self.records)


# (role, case, view, expectation): "none" means not one request with request bytes; "some" means at least one.
TARGET_BLOCKED = ("initial", "fetch", "image", "script", "frameDocument", "frameSameFetch", "frameSameImage",
                  "frameCrossFetch", "frameCrossImage", "cancelStillBlocked", "unsupportedStillBlocked",
                  "xhr", "style", "eventSource", "audio", "beacon", "noCorsPost", "formGet", "formPost",
                  "popup", "deferredPopup", "mainFormGet", "mainFormPost", "reload")
EXPECTATIONS = (
    *((("target", case, "A", "none")) for case in TARGET_BLOCKED),
    *((("target", case, "B", "some")) for case in TARGET_BLOCKED),
    ("source", "allowedSource", "A", "some"), ("source", "allowedSource", "B", "some"),
    ("source", "cancelAllowed", "A", "some"),
    ("target", "replacementOldAllowed", "A", "some"),
    ("cross", "replacementBlocked", "A", "none"), ("cross", "replacementBlocked", "B", "some"),
    ("cross", "clearAllowed", "A", "some"),
)


def request_matches(record: dict, case: str, view: str) -> bool:
    """A request counts as soon as its bytes arrived, even when the request line was cut off before the query parsed."""
    query = record.get("query")
    if query is not None:
        return query.get("case") == case and query.get("v") == view
    line = record.get("request_line", "")
    return bool(re.search(rf"[?&]v={view}(&|\s|$)", line)) and bool(re.search(rf"[?&]case={re.escape(case)}(&|\s|$)", line))


def count_requests(records: list[dict]) -> dict[str, int]:
    counts = {}
    for role, case, view, _ in EXPECTATIONS:
        counts[f"{role}/{case}/{view}"] = len([record for record in records if record["role"] == role and record["bytes"]
                                                 and request_matches(record, case, view)])
    return counts


def verify_host_counters(records: list[dict]) -> list[str]:
    """What reached the host must agree with the activity: no request bytes of a refused case of view A, at least one
    request of each allowed case and of each counter-check of the unfiltered view B."""
    counts = count_requests(records)
    problems = [f"{record['role']} received {record['bytes']} unclassifiable partial HTTP bytes"
                for record in records if record["bytes"] and not record.get("headers_complete", False)]
    for role, case, view, expectation in EXPECTATIONS:
        count = counts[f"{role}/{case}/{view}"]
        if expectation == "none" and count:
            problems.append(f"{role} received {count} request(s) of {case} v={view}, expected none")
        if expectation == "some" and not count:
            problems.append(f"{role} received no request of {case} v={view}")
    return problems


# --- Emulator --------------------------------------------------------------------------------------------------------

def free_console_port(start: int) -> int:
    for port in range(start, 5682, 2):
        busy = False
        for candidate in (port, port + 1):
            with socket.socket() as probe:
                if probe.connect_ex(("127.0.0.1", candidate)) == 0:
                    busy = True
        if not busy:
            return port
    raise RuntimeError("no free emulator console port")


class Emulator:
    def __init__(self, sdk: Path, api: str, port: int, work: Path, logs: Path) -> None:
        self.sdk, self.api, self.port, self.logs = sdk, api, port, logs
        self.serial = f"emulator-{port}"
        self.env = dict(os.environ, ANDROID_SDK_ROOT=str(sdk), ANDROID_HOME=str(sdk), ANDROID_AVD_HOME=str(work / "avd"))
        (work / "avd").mkdir(parents=True)
        self.name = f"neoastra_filtertest_api{api.replace('.', '_')}"
        self.process: subprocess.Popen | None = None
        self.nvram = Path.home() / ".android" / f"modem-nv-ram-{port}"
        self.nvram_existed = self.nvram.exists()

    def adb(self, *arguments: str, timeout: int = 120, check: bool = True) -> str:
        return run(self.sdk / "platform-tools/adb", "-s", self.serial, *arguments, timeout=timeout, env=self.env, check=check)

    def start(self) -> None:
        image = f"system-images;android-{self.api};google_apis;arm64-v8a"
        if not (self.sdk / "system-images" / f"android-{self.api}" / "google_apis" / "arm64-v8a").is_dir():
            raise RuntimeError(f"system image {image} is not installed (no installation)")
        avdmanager = self.sdk / "cmdline-tools/latest/bin/avdmanager"
        subprocess.run([str(avdmanager), "create", "avd", "-n", self.name, "-k", image, "--force"], input="no\n", text=True,
                       env=self.env, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
        output = open(self.logs / "emulator.log", "w", encoding="utf-8")
        self.process = subprocess.Popen(
            [str(self.sdk / "emulator/emulator"), "-avd", self.name, "-port", str(self.port), "-no-window", "-no-audio",
             "-no-snapshot", "-no-boot-anim", "-wipe-data", "-gpu", "swiftshader_indirect"],
            env=self.env, stdout=output, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 600
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise RuntimeError(f"emulator exited with {self.process.returncode}; see {self.logs / 'emulator.log'}")
            booted = subprocess.run([str(self.sdk / "platform-tools/adb"), "-s", self.serial, "shell", "getprop",
                                     "sys.boot_completed"], capture_output=True, text=True, env=self.env)
            if booted.stdout.strip() == "1":
                return
            time.sleep(2)
        raise RuntimeError("emulator did not boot within 10 minutes")

    def stop(self) -> None:
        if self.process is not None and self.process.poll() is None:
            subprocess.run([str(self.sdk / "platform-tools/adb"), "-s", self.serial, "emu", "kill"], capture_output=True,
                           env=self.env)
            try:
                self.process.wait(timeout=60)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        if not self.nvram_existed:
            self.nvram.unlink(missing_ok=True)


def run_probe(sdk: Path, apk: Path, api: str, port: int, timeout: int, work: Path,
              tls_context: ssl.SSLContext | None = None) -> bool:
    logs = LOG_ROOT / (f"api{api}" + ("-https" if tls_context is not None else ""))
    logs.mkdir(parents=True, exist_ok=True)
    for stale in ("result.txt", "logcat.txt", "nafilter.txt", "server-records.jsonl", "summary.json", "device.txt"):
        (logs / stale).unlink(missing_ok=True)
    emulator = Emulator(sdk, api, port, work / f"api{api}", logs)
    servers: HostServers | None = None
    summary: dict = {"api": api, "serial": emulator.serial, "transport": "https" if tls_context is not None else "http"}
    try:
        emulator.start()
        device = emulator.adb("shell", "getprop", "ro.build.fingerprint")
        device += emulator.adb("shell", "dumpsys", "webviewupdate", check=False)
        (logs / "device.txt").write_text(device, encoding="utf-8")
        emulator.adb("install", "-r", "-t", str(apk), timeout=300)
        emulator.adb("logcat", "-c")
        servers = HostServers(tls_context)
        # Match NeoMobileDevelopment's local-dev transport rather than depending
        # on the emulator's asynchronously initialized external network route.
        for listener in servers.sockets.values():
            port_name = f"tcp:{listener.getsockname()[1]}"
            emulator.adb("reverse", port_name, port_name)
        origins = {role: servers.origin(role) for role in HostServers.ROLES}
        summary["origins"] = origins
        emulator.adb("shell", "am", "start", "-W", "-n", ACTIVITY, "--es", "source", origins["source"], "--es", "cross",
                     origins["cross"], "--es", "target", origins["target"])
        # The activity writes files/result.txt last: its content ends the wait, not the start of the activity.
        def read_result() -> str | None:
            result = subprocess.run([str(sdk / "platform-tools/adb"), "-s", emulator.serial, "exec-out", "run-as", PACKAGE,
                                     "cat", "files/result.txt"], capture_output=True, text=True, env=emulator.env)
            text = result.stdout.strip()
            return text if result.returncode == 0 and text in ("PASS", "FAIL") else None

        result_text = None
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline and result_text is None:
            time.sleep(2)
            result_text = read_result()
        time.sleep(1)
        # Filter on the device: a fresh emulator can flood the full dump with boot
        # messages and terminate/truncate adb's stream before our verdict arrives.
        logcat_result = subprocess.run([str(sdk / "platform-tools/adb"), "-s", emulator.serial, "logcat", "-d", "-v",
                                       "threadtime", "NAFILTER:V", "*:S"],
                                      capture_output=True, text=True, env=emulator.env, timeout=120)
        logcat = logcat_result.stdout
        (logs / "logcat.txt").write_text(logcat, encoding="utf-8")
        (logs / "logcat-stderr.txt").write_text(logcat_result.stderr, encoding="utf-8")
        logcat_result.check_returncode()
        tagged = [line for line in logcat.splitlines() if re.search(r"\bNAFILTER\s*:", line)]
        (logs / "nafilter.txt").write_text("\n".join(tagged) + "\n", encoding="utf-8")
        # The final verdict is a NAFILTER message that is exactly PASS or FAIL; lines about single cases do not count.
        verdicts = [match.group(1) for match in (re.search(r"\bNAFILTER\s*:\s*(PASS|FAIL)\s*$", line) for line in tagged) if match]
        verdict = verdicts[-1] if verdicts else None
        if result_text is not None:
            (logs / "result.txt").write_text(result_text + "\n", encoding="utf-8")
        records = servers.close()
        servers = None
        with open(logs / "server-records.jsonl", "w", encoding="utf-8") as output:
            for record in records:
                output.write(json.dumps(record) + "\n")
        problems = verify_host_counters(records)
        if verdict is None:
            problems.append("no NAFILTER message that is exactly PASS or FAIL in logcat")
        elif verdict != "PASS":
            problems.append(f"activity reported NAFILTER {verdict}")
        if result_text != "PASS":
            problems.append(f"files/result.txt is {result_text!r} after up to {timeout} s, not 'PASS'")
        summary.update({"host_counts": count_requests(records), "logcat_verdict": verdict, "result_txt": result_text, "connections": len(records),
                        "empty_connections": len([record for record in records if not record["bytes"]]),
                        "tls_established": sum(record["tls_established"] for record in records),
                        "tls_errors": sum(bool(record.get("tls_error")) for record in records),
                        "problems": problems, "passed": not problems})
        return not problems
    except Exception as error:  # the summary records why a run could not finish; the caller fails the run
        summary.update({"passed": False, "problems": [f"{type(error).__name__}: {error}"]})
        return False
    finally:
        if servers is not None:
            servers.close()
        emulator.stop()
        (logs / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
        log(f"{'PASS' if summary.get('passed') else 'FAIL'} Android request-filter API {api}: {summary.get('problems')}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--api", action="append", help="system image API level, repeatable (default: 36 and 37.0)")
    parser.add_argument("--port", type=int, default=5650, help="first emulator console port to try (even, default 5650)")
    parser.add_argument("--timeout", type=int, default=180, help="seconds to wait for files/result.txt")
    parser.add_argument("--skip-native-build", action="store_true", help="use the libraries already in the build folder")
    parser.add_argument("--build-only", action="store_true", help="build the APK and keep it in the log folder, run nothing")
    parser.add_argument("--java-only", action="store_true", help="only resolve dependencies, link resources and compile Java")
    parser.add_argument("--https", action="store_true", help="use TLS with a disposable CA trusted only by this probe APK")
    args = parser.parse_args()
    sdk = Path(os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME") or Path.home() / "Library/Android/sdk")
    env = dict(os.environ, ANDROID_NDK_HOME=os.environ.get("ANDROID_NDK_HOME") or str(sdk / "ndk" / NDK))
    LOG_ROOT.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="neoastra-android-filter-") as temporary:
        work = Path(temporary)
        tls_ca = None
        tls_context = None
        if args.https:
            tls_ca, certificate, key = create_tls_fixture(work / "tls")
            tls_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            tls_context.load_cert_chain(certificate, key)
        if not (args.java_only or args.skip_native_build):
            build_native(env)
        (work / "apk").mkdir()
        apk = build_apk(sdk, work / "apk", args.java_only, tls_ca)
        if args.java_only:
            log("Java sources compile against the cached AndroidX libraries")
            return 0
        if args.build_only:
            shutil.copy(apk, LOG_ROOT / "probe.apk")
            log(f"APK {LOG_ROOT / 'probe.apk'}")
            return 0
        passed = True
        port = args.port
        for api in args.api or ["36", "37.0"]:
            port = free_console_port(port)
            passed = run_probe(sdk, apk, api, port, args.timeout, work, tls_context) and passed
            port += 2
        return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
