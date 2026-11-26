package com.neoastra.backend;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

/** Runs without Android; checks ownership across request-thread versus UI-thread disposal. */
public final class NeoAstraRequestFilterGateCheck {
    public static void main(String[] arguments) throws Exception {
        AtomicInteger calls = new AtomicInteger();
        NeoAstraRequestFilterGate gate = new NeoAstraRequestFilterGate(42, (view, uri, main) -> {
            expect(view == 42, "native identity is preserved");
            calls.incrementAndGet();
            return uri.endsWith("/blocked");
        });
        expect(!gate.refuses("https://example.test/allowed", true), "allowed target");
        expect(gate.refuses("https://example.test/blocked", false), "blocked subresource");
        gate.close();
        gate.close();
        expect(gate.refuses("https://example.test/allowed", false), "closed view refuses requests");
        expect(calls.get() == 2, "closed view never calls native code again");

        // The old native create signature never enables the new JNI method.
        NeoAstraRequestFilterGate legacy = new NeoAstraRequestFilterGate(43, null);
        expect(!legacy.refuses("https://example.test/allowed", true), "old native library stays compatible");
        legacy.close();
        expect(legacy.refuses("https://example.test/allowed", true), "old-library disposal also closes the gate");

        CountDownLatch entered = new CountDownLatch(1), leave = new CountDownLatch(1);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        NeoAstraRequestFilterGate racing = new NeoAstraRequestFilterGate(44, (view, uri, main) -> {
            entered.countDown();
            try { expect(leave.await(5, TimeUnit.SECONDS), "release concurrent request"); }
            catch (InterruptedException error) { throw new AssertionError(error); }
            return true;
        });
        Thread request = new Thread(() -> {
            try { expect(racing.refuses("https://example.test/blocked", false), "in-flight result"); }
            catch (Throwable error) { failure.set(error); }
        });
        request.start();
        expect(entered.await(5, TimeUnit.SECONDS), "request entered native checker");
        Thread dispose = new Thread(racing::close);
        dispose.start();
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (dispose.getState() != Thread.State.BLOCKED && System.nanoTime() < deadline) Thread.yield();
        expect(dispose.getState() == Thread.State.BLOCKED, "disposal waits for the synchronous native check");
        leave.countDown();
        request.join(5000); dispose.join(5000);
        expect(!request.isAlive() && !dispose.isAlive() && failure.get() == null, "concurrent calls finish");
        expect(racing.refuses("https://example.test/anything", true), "no callback after racing disposal");
        System.out.println("NeoAstraRequestFilterGate: ok");
    }

    private static void expect(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
}
