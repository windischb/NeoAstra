package com.neoastra.backend;

/**
 * Keeps a request's native pointer valid until its synchronous filter check
 * returns. Closing on the UI thread drains that check before native storage
 * can be freed/reused; no request is marshaled to the UI thread.
 */
final class NeoAstraRequestFilterGate {
    interface Checker { boolean refuses(long view, String uri, boolean mainFrame); }

    private long view;
    private final Checker checker;

    NeoAstraRequestFilterGate(long view, Checker checker) {
        this.view = view;
        this.checker = checker;
    }

    synchronized boolean refuses(String uri, boolean mainFrame) {
        return view == 0 || (checker != null && checker.refuses(view, uri, mainFrame));
    }

    synchronized void close() { view = 0; }
}
