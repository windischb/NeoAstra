using System.Net;
using System.Runtime.InteropServices;
using NeoAstra;
using NeoAstra.Tests;

internal static partial class Program
{
    private sealed partial class ConformanceSuite
    {
        private static void EnableGtkProbePopups(NeoAstra.NeoAstra view)
        {
            var native = view.GetNativeHandle(NeoNativeHandleKind.WebKitGtkWebView).Value;
            var settings = GtkProbeNative.webkit_web_view_get_settings(native);
            Require(settings != 0, "The GTK popup probe has no settings.");
            GtkProbeNative.webkit_settings_set_javascript_can_open_windows_automatically(settings, 1);
        }

        private static void AllowGtkProbeCertificate(NeoAstra.NeoAstra view, RequestFilterTlsServer fixture)
        {
            Require(IPAddress.TryParse(fixture.Origin.Host, out var address) && IPAddress.IsLoopback(address),
                "GTK test certificate exceptions must remain loopback-only.");
            var native = view.GetNativeHandle(NeoNativeHandleKind.WebKitGtkWebView).Value;
            var session = GtkProbeNative.webkit_web_view_get_network_session(native);
            Require(session != 0 && GtkProbeNative.webkit_network_session_is_ephemeral(session) != 0,
                "GTK certificate setup requires a disposable private network session.");
            var certificate = GtkProbeNative.g_tls_certificate_new_from_pem(fixture.Certificate.ExportCertificatePem(), -1, out var error);
            try
            {
                Require(error == 0 && certificate != 0, "GTK could not import the fixture certificate.");
                GtkProbeNative.webkit_network_session_allow_tls_certificate_for_host(session, certificate, fixture.Origin.Host);
            }
            finally
            {
                if (error != 0) GtkProbeNative.g_error_free(error);
                if (certificate != 0) GtkProbeNative.g_object_unref(certificate);
            }
        }

        // Test-only calls to public GTK/GLib APIs. There is no environment-wide
        // "ignore TLS" switch and no write to a user's or system's trust store.
        private static partial class GtkProbeNative
        {
            [LibraryImport("libwebkitgtk-6.0.so.4")]
            internal static partial nint webkit_web_view_get_settings(nint view);

            [LibraryImport("libwebkitgtk-6.0.so.4")]
            internal static partial void webkit_settings_set_javascript_can_open_windows_automatically(nint settings, int enabled);

            [LibraryImport("libwebkitgtk-6.0.so.4")]
            internal static partial nint webkit_web_view_get_network_session(nint view);

            [LibraryImport("libwebkitgtk-6.0.so.4")]
            internal static partial int webkit_network_session_is_ephemeral(nint session);

            [LibraryImport("libwebkitgtk-6.0.so.4", StringMarshalling = StringMarshalling.Utf8)]
            internal static partial void webkit_network_session_allow_tls_certificate_for_host(nint session, nint certificate, string host);

            [LibraryImport("libgio-2.0.so.0", StringMarshalling = StringMarshalling.Utf8)]
            internal static partial nint g_tls_certificate_new_from_pem(string pem, nint length, out nint error);

            [LibraryImport("libglib-2.0.so.0")]
            internal static partial void g_error_free(nint error);

            [LibraryImport("libgobject-2.0.so.0")]
            internal static partial void g_object_unref(nint value);
        }
    }
}
