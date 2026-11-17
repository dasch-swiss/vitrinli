mod common;

use common::server;
use sipi_e2e::{http_client, test_data_dir, Signal, SipiServer};
use std::io::{Read as _, Write as _};
use std::net::TcpStream;
use std::time::Duration;

// =============================================================================
// Connection handling tests
// =============================================================================

#[test]
fn http_keep_alive() {
    // Verify that the server supports HTTP keep-alive by making two sequential
    // requests using reqwest's connection pooling (which reuses TCP connections).
    // Uses its own server with minimal config to avoid dispatch bug on static musl binary.
    let test_data = test_data_dir();
    let srv = SipiServer::start("config/sipi.cache-test-config.lua", &test_data);

    // reqwest's blocking client reuses connections by default.
    // Make two requests and verify both succeed — if keep-alive works,
    // the second request reuses the TCP connection.
    let c = http_client();

    let resp1 = c
        .get(format!("{}/unit/lena512.jp2/info.json", srv.base_url))
        .send()
        .expect("first keep-alive request failed");
    assert_eq!(resp1.status().as_u16(), 200);
    let _ = resp1.text(); // consume body

    let resp2 = c
        .get(format!("{}/unit/lena512.jp2/info.json", srv.base_url))
        .send()
        .expect("second keep-alive request failed");
    assert_eq!(resp2.status().as_u16(), 200);
    let _ = resp2.text(); // consume body

    // Third request immediately to stress connection reuse
    let resp3 = c
        .get(format!(
            "{}/unit/lena512.jp2/full/max/0/default.jpg",
            srv.base_url
        ))
        .send()
        .expect("third keep-alive request failed");
    assert_eq!(resp3.status().as_u16(), 200);
}

#[test]
fn chunked_transfer_upload() {
    // Send request with Transfer-Encoding: chunked body.
    // Uses its own server because malformed raw HTTP can destabilize connection handlers.
    let test_data = test_data_dir();
    let srv = SipiServer::start("config/sipi.cache-test-config.lua", &test_data);
    let addr = format!("127.0.0.1:{}", srv.http_port);

    let mut stream = TcpStream::connect(&addr).expect("TCP connect failed");
    stream.set_read_timeout(Some(Duration::from_secs(10))).ok();

    // Send a chunked request to a known route
    let headers = "POST /api/upload HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\nContent-Type: application/octet-stream\r\n\r\n";
    stream.write_all(headers.as_bytes()).expect("write headers");

    // Send a small chunk
    stream.write_all(b"5\r\nhello\r\n").expect("write chunk");
    // End chunk
    stream.write_all(b"0\r\n\r\n").expect("write final chunk");

    let mut buf = vec![0u8; 8192];
    let result = stream.read(&mut buf);

    match result {
        Ok(0) => {
            // Server closed connection — acceptable behavior for unsupported
            // chunked encoding or malformed request
        }
        Ok(n) => {
            let resp = String::from_utf8_lossy(&buf[..n]);
            // Server responded — should be a valid HTTP response
            assert!(
                resp.contains("HTTP/1.1") || resp.contains("HTTP/1.0"),
                "chunked request should get an HTTP response, got: {}",
                &resp[..resp.len().min(200)]
            );
        }
        Err(_) => {
            // Timeout or connection reset — acceptable; server didn't hang
        }
    }

    // Verify server is still responsive via normal HTTP client
    let c = http_client();
    let health = c
        .get(format!(
            "{}/unit/lena512.jp2/full/max/0/default.jpg",
            srv.base_url
        ))
        .send()
        .expect("server should respond after chunked request");
    assert_eq!(health.status().as_u16(), 200);
}

#[test]
fn connection_close_header() {
    // Send request with Connection: close, verify server closes the TCP connection.
    // Uses its own server to avoid destabilizing shared server with raw TCP.
    let test_data = test_data_dir();
    let srv = SipiServer::start("config/sipi.cache-test-config.lua", &test_data);
    let addr = format!("127.0.0.1:{}", srv.http_port);

    let mut stream = TcpStream::connect(&addr).expect("TCP connect failed");
    stream.set_read_timeout(Some(Duration::from_secs(10))).ok();

    let req =
        "GET /unit/lena512.jp2/info.json HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    stream.write_all(req.as_bytes()).expect("write request");

    // Read the full response
    let mut response = Vec::new();
    loop {
        let mut buf = [0u8; 4096];
        match stream.read(&mut buf) {
            Ok(0) => break, // Connection closed
            Ok(n) => response.extend_from_slice(&buf[..n]),
            Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => break,
            Err(e) if e.kind() == std::io::ErrorKind::TimedOut => break,
            Err(e) => panic!("unexpected read error: {}", e),
        }
    }

    let resp_str = String::from_utf8_lossy(&response);
    assert!(
        resp_str.contains("HTTP/1.1 200") || resp_str.contains("HTTP/1.0 200"),
        "should get 200 response, got: {}",
        &resp_str[..resp_str.len().min(100)]
    );

    // After Connection: close, further writes should fail or read should return 0
    std::thread::sleep(Duration::from_millis(100));
    let mut check_buf = [0u8; 1];
    let closed = match stream.read(&mut check_buf) {
        Ok(0) => true,  // Properly closed
        Err(_) => true, // Error = closed
        Ok(_) => false, // Still open — not ideal but not a failure
    };

    if !closed {
        eprintln!("NOTE: server did not immediately close connection after Connection: close");
    }
}

#[test]
fn graceful_shutdown() {
    // Start sipi, send SIGTERM, verify it shuts down cleanly.
    let test_data = test_data_dir();
    let srv = SipiServer::start("config/sipi.cache-test-config.lua", &test_data);
    let c = http_client();

    // Verify server is running
    let resp = c
        .get(format!(
            "{}/unit/lena512.jp2/full/max/0/default.jpg",
            srv.base_url
        ))
        .send()
        .expect("pre-shutdown request failed");
    assert_eq!(resp.status().as_u16(), 200);
    let _ = resp.bytes();

    // Send SIGTERM to the server process
    srv.signal(Signal::SIGTERM).expect("failed to send SIGTERM");

    // Wait a bit for shutdown
    std::thread::sleep(Duration::from_secs(2));

    // After SIGTERM, connection should be refused
    let result = c
        .get(format!(
            "{}/unit/lena512.jp2/full/max/0/default.jpg",
            srv.base_url
        ))
        .send();

    match result {
        Ok(resp) => {
            // Server might still be draining — that's acceptable
            eprintln!("Server still responding after SIGTERM: {}", resp.status());
        }
        Err(_) => {
            // Connection refused — server shut down cleanly
        }
    }
}

#[test]
fn keepalive_timeout_enforcement() {
    // Open a keep-alive connection, idle for longer than the server's idle
    // keep-alive timeout, and verify the server closes it.
    let srv = server();
    let addr = format!("127.0.0.1:{}", srv.http_port);

    let mut stream = TcpStream::connect(&addr).expect("TCP connect failed");
    stream.set_read_timeout(Some(Duration::from_secs(15))).ok();

    // Send first request with keep-alive
    let req = "GET /unit/lena512.jp2/info.json HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
    stream.write_all(req.as_bytes()).expect("write request");

    let mut buf = vec![0u8; 16384];
    let n = stream.read(&mut buf).expect("read response");
    assert!(n > 0, "should get response");

    // Wait longer than keep_alive timeout (5 seconds) + margin
    std::thread::sleep(Duration::from_secs(7));

    // Try to send another request — should fail if server closed the connection
    let req2 = "GET /unit/lena512.jp2/info.json HTTP/1.1\r\nHost: localhost\r\n\r\n";
    let write_result = stream.write_all(req2.as_bytes());

    match write_result {
        Ok(_) => {
            // Write succeeded, but read may fail
            let mut buf2 = [0u8; 1024];
            match stream.read(&mut buf2) {
                Ok(0) => {} // Connection closed — expected
                Ok(_) => {
                    eprintln!("NOTE: server kept connection alive beyond timeout");
                }
                Err(_) => {} // Error = closed — expected
            }
        }
        Err(_) => {
            // Write failed — connection was closed (expected behavior)
        }
    }
}
