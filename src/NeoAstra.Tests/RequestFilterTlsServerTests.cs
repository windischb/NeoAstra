using System.Net.Security;
using System.Net.Sockets;
using System.Security.Cryptography.X509Certificates;

namespace NeoAstra.Tests;

[TestClass]
public sealed class RequestFilterTlsServerTests
{
    [TestMethod]
    public async Task FrameDocumentIsSelectedOnlyByItsExactPath()
    {
        await using var server = new RequestFilterTlsServer("<body>frame-ä</body>");
        using var handler = new HttpClientHandler
        {
            ServerCertificateCustomValidationCallback = (_, certificate, _, _) =>
                certificate is not null && certificate.RawData.AsSpan().SequenceEqual(server.Certificate.RawData),
        };
        using var client = new HttpClient(handler);
        Assert.AreEqual("<body>frame-ä</body>", await client.GetStringAsync(new Uri(server.Origin, "frame")));
        StringAssert.Contains(await client.GetStringAsync(new Uri(server.Origin, "frame-other")), "neoastra-tls-ready");
        Assert.AreEqual(2, server.Requests);
    }

    [TestMethod]
    public async Task UntrustedCertificateFails_AndFixturePinnedClientReachesHttp()
    {
        await using var server = new RequestFilterTlsServer();
        using var untrusted = new HttpClient();
        await Assert.ThrowsAsync<HttpRequestException>(() => untrusted.GetAsync(server.Origin));
        Assert.AreEqual(0L, server.Bytes);
        using var handler = new HttpClientHandler
        {
            ServerCertificateCustomValidationCallback = (_, certificate, _, _) =>
                certificate is not null && certificate.RawData.AsSpan().SequenceEqual(server.Certificate.RawData),
        };
        using var trusted = new HttpClient(handler);
        StringAssert.Contains(await trusted.GetStringAsync(server.Origin), "neoastra-tls-ready");
        Assert.AreEqual(1, server.Requests);
        Assert.IsGreaterThan(0L, server.Bytes);
    }

    [TestMethod]
    public async Task PartialHttpBytesAreCountedAfterTls_WithoutACompleteRequest()
    {
        var server = new RequestFilterTlsServer();
        try
        {
            using var client = new TcpClient();
            await client.ConnectAsync(server.Origin.Host, server.Origin.Port);
            await using var stream = new SslStream(client.GetStream(), false, (_, certificate, _, _) =>
                certificate is not null && certificate.GetRawCertData().AsSpan().SequenceEqual(server.Certificate.RawData));
            await stream.AuthenticateAsClientAsync(new SslClientAuthenticationOptions
            {
                TargetHost = server.Origin.Host,
                CertificateRevocationCheckMode = X509RevocationMode.NoCheck,
            });
            await stream.WriteAsync("G"u8.ToArray());
        }
        finally
        {
            await server.DisposeAsync();
        }
        Assert.AreEqual(1L, server.Bytes);
        Assert.AreEqual(0, server.Requests);
        Assert.AreEqual(1, server.Handshakes);
    }
}
