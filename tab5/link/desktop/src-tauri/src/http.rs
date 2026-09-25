//! The smallest HTTP client that talks to the Link on this PC.
//!
//! Only ever `127.0.0.1`, only ever the Link's own JSON answers (which always carry a Content-Length),
//! and `Connection: close`, so reading to the end is reading the answer. A dependency would buy nothing.

use std::io::{Read, Write};
use std::net::{SocketAddr, TcpStream};
use std::time::Duration;

pub struct Response {
    pub status: u16,
    pub body: Vec<u8>,
}

pub fn request(
    port: u16,
    method: &str,
    path: &str,
    token: Option<&str>,
    body: Option<&[u8]>,
    timeout: Duration,
) -> Result<Response, String> {
    let addr = SocketAddr::from(([127, 0, 0, 1], port));
    let mut stream = TcpStream::connect_timeout(&addr, timeout).map_err(|e| e.to_string())?;
    stream.set_read_timeout(Some(timeout)).ok();
    stream.set_write_timeout(Some(timeout)).ok();

    let body = body.unwrap_or(&[]);
    let mut head = format!(
        "{method} {path} HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nConnection: close\r\nAccept: application/json\r\nUser-Agent: CatalystLinkDesktop/{}\r\n",
        env!("CARGO_PKG_VERSION")
    );
    if let Some(t) = token {
        head.push_str(&format!("X-Link-Token: {t}\r\n"));
    }
    if method == "POST" {
        head.push_str(&format!("Content-Type: application/json\r\nContent-Length: {}\r\n", body.len()));
    }
    head.push_str("\r\n");
    stream.write_all(head.as_bytes()).map_err(|e| e.to_string())?;
    if method == "POST" {
        stream.write_all(body).map_err(|e| e.to_string())?;
    }

    let mut raw = Vec::new();
    stream.read_to_end(&mut raw).map_err(|e| e.to_string())?;
    parse(&raw)
}

fn parse(raw: &[u8]) -> Result<Response, String> {
    let split = raw
        .windows(4)
        .position(|w| w == b"\r\n\r\n")
        .ok_or("the Link's answer had no header end")?;
    let head = String::from_utf8_lossy(&raw[..split]);
    let mut lines = head.lines();
    let status = lines
        .next()
        .and_then(|l| l.split_whitespace().nth(1))
        .and_then(|s| s.parse::<u16>().ok())
        .ok_or("the Link's answer had no status")?;
    let mut body = raw[split + 4..].to_vec();
    for line in lines {
        if let Some((k, v)) = line.split_once(':') {
            if k.trim().eq_ignore_ascii_case("content-length") {
                if let Ok(n) = v.trim().parse::<usize>() {
                    body.truncate(n);
                }
            }
        }
    }
    Ok(Response { status, body })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_status_and_body() {
        let r = parse(b"HTTP/1.1 401 Unauthorized\r\nContent-Length: 5\r\n\r\nhelloEXTRA").unwrap();
        assert_eq!(r.status, 401);
        assert_eq!(r.body, b"hello");
    }

    #[test]
    fn refuses_garbage() {
        assert!(parse(b"nope").is_err());
    }
}
