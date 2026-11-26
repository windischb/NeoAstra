using System.Collections.Concurrent;
using System.Net;
using System.Net.Security;
using System.Net.Sockets;
using System.Security.Authentication;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;

namespace NeoAstra.Tests;

// A disposable HTTPS origin for filter probes. Transport accepts/handshakes are
// separate from decoded HTTP bytes; even an incomplete request counts as a leak.
internal sealed class RequestFilterTlsServer : IAsyncDisposable
{
    private readonly TcpListener _listener = new(IPAddress.Loopback, 0);
    private readonly CancellationTokenSource _stopping = new();
    private readonly ConcurrentBag<Task> _clients = [];
    private readonly Task _accepting;
    private readonly byte[] _response;
    private readonly byte[] _frameResponse;
    private long _bytes;
    private int _connections;
    private int _handshakes;
    private int _requests;

    internal RequestFilterTlsServer(string? frameDocument = null)
    {
        using var key = RSA.Create(2048);
        var request = new CertificateRequest("CN=NeoAstra disposable filter probe", key, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
        var names = new SubjectAlternativeNameBuilder();
        names.AddIpAddress(IPAddress.Loopback);
        request.CertificateExtensions.Add(names.Build());
        Certificate = request.CreateSelfSigned(DateTimeOffset.UtcNow.AddMinutes(-1), DateTimeOffset.UtcNow.AddDays(1));
        _response = Response("<!doctype html><body>neoastra-tls-ready</body>");
        _frameResponse = frameDocument is null ? _response : Response(frameDocument);
        _listener.Start();
        Origin = new Uri($"https://127.0.0.1:{((IPEndPoint)_listener.LocalEndpoint).Port}/");
        _accepting = AcceptAsync();
    }

    internal Uri Origin { get; }
    internal X509Certificate2 Certificate { get; }
    internal long Bytes => Interlocked.Read(ref _bytes);
    internal int Requests => Volatile.Read(ref _requests);
    internal int Connections => Volatile.Read(ref _connections);
    internal int Handshakes => Volatile.Read(ref _handshakes);

    private static byte[] Response(string html)
    {
        var body = Encoding.UTF8.GetBytes(html);
        return [.. Encoding.ASCII.GetBytes($"HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: {body.Length}\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n"), .. body];
    }

    private async Task AcceptAsync()
    {
        try
        {
            while (true)
            {
                var client = await _listener.AcceptTcpClientAsync(_stopping.Token).ConfigureAwait(false);
                Interlocked.Increment(ref _connections);
                _clients.Add(ServeAsync(client));
            }
        }
        catch (OperationCanceledException) when (_stopping.IsCancellationRequested) { }
    }

    private async Task ServeAsync(TcpClient client)
    {
        using var owned = client;
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(3));
        await using var stream = new SslStream(client.GetStream());
        try
        {
            await stream.AuthenticateAsServerAsync(new SslServerAuthenticationOptions
            {
                ServerCertificate = Certificate,
            }, timeout.Token).ConfigureAwait(false);
            Interlocked.Increment(ref _handshakes);
            var buffer = new byte[16384];
            var length = 0;
            while (length < buffer.Length)
            {
                var read = await stream.ReadAsync(buffer.AsMemory(length), timeout.Token).ConfigureAwait(false);
                if (read == 0) return;
                Interlocked.Add(ref _bytes, read);
                length += read;
                if (buffer.AsSpan(0, length).IndexOf("\r\n\r\n"u8) < 0) continue;
                Interlocked.Increment(ref _requests);
                var response = buffer.AsSpan(0, length).StartsWith("GET /frame "u8) ? _frameResponse : _response;
                await stream.WriteAsync(response, timeout.Token).ConfigureAwait(false);
                return;
            }
        }
        catch (Exception error) when (error is AuthenticationException or IOException or OperationCanceledException) { }
    }

    public async ValueTask DisposeAsync()
    {
        await _stopping.CancelAsync().ConfigureAwait(false);
        await _accepting.ConfigureAwait(false);
        _listener.Stop();
        // Each accepted client has its own bounded timeout. Don't cancel a read
        // and then take a snapshot before already-received bytes were counted.
        await Task.WhenAll(_clients).ConfigureAwait(false);
        Certificate.Dispose();
        _stopping.Dispose();
    }
}
