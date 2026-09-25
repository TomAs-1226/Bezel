//! What the log panel never shows.
//!
//! The Link runs with `--gui`, which already keeps the token and pairing codes off its console. This is
//! the second lock on the same door: every line is masked here before it is stored, so a token printed
//! by anything (a traceback quoting a header, a future print someone adds) still never reaches the
//! window, the clipboard or a screenshot.

use regex::Regex;
use std::sync::OnceLock;

struct Rules {
    token: Regex,
    header: Regex,
    api_key: Regex,
    oauth: Regex,
    pairing: Regex,
    code: Regex,
}

fn rules() -> &'static Rules {
    static RULES: OnceLock<Rules> = OnceLock::new();
    RULES.get_or_init(|| Rules {
        // The Link's tokens: four groups of four from its touch-keyboard alphabet (state.py)
        token: Regex::new(r"\b[a-hjkmnp-z2-9]{4}-[a-hjkmnp-z2-9]{4}-[a-hjkmnp-z2-9]{4}-[a-hjkmnp-z2-9]{4}\b").unwrap(),
        header: Regex::new(r#"(?i)(x-link-token|authorization|x-api-key)(\W{1,4})[^\s,'"}]+"#).unwrap(),
        api_key: Regex::new(r"sk-ant-[A-Za-z0-9_\-]+").unwrap(),
        oauth: Regex::new(r"(?i)(CLAUDE_CODE_OAUTH_TOKEN|ANTHROPIC_API_KEY)(\s*[=:]\s*)\S+").unwrap(),
        pairing: Regex::new(r"(?i)pair|code").unwrap(),
        code: Regex::new(r"\b\d{3}[ \-]?\d{3}\b").unwrap(),
    })
}

pub fn redact(line: &str) -> String {
    let r = rules();
    let mut out = r.token.replace_all(line, "••••-••••-••••-••••").into_owned();
    out = r.header.replace_all(&out, "$1$2••••").into_owned();
    out = r.api_key.replace_all(&out, "sk-ant-••••").into_owned();
    out = r.oauth.replace_all(&out, "$1$2••••").into_owned();
    // A six-digit number on a line about pairing is a code until proven otherwise.
    if r.pairing.is_match(&out) {
        out = r.code.replace_all(&out, "••• •••").into_owned();
    }
    out
}

#[cfg(test)]
mod tests {
    use super::redact;

    #[test]
    fn masks_tokens() {
        let s = redact("  token    k7mq-3xtr-9pwd-h2fc   (or type this)");
        assert!(!s.contains("k7mq"), "{s}");
        assert!(s.contains("••••-••••"));
    }

    #[test]
    fn masks_pairing_codes_but_not_other_numbers() {
        let s = redact("pairing catalyst tab: enter  482 913  on the tablet");
        assert!(!s.contains("482"), "{s}");
        let s = redact("[link] 192.168.0.9 \"GET /inbox HTTP/1.1\" 200 123456");
        assert!(s.contains("123456"), "{s}");
    }

    #[test]
    fn masks_headers_and_keys() {
        assert!(!redact("X-Link-Token: abcdefgh").contains("abcdefgh"));
        assert!(!redact("key sk-ant-api03-AbC_d-1").contains("AbC"));
        assert!(!redact("CLAUDE_CODE_OAUTH_TOKEN=sk-xyz").contains("sk-xyz"));
    }

    #[test]
    fn leaves_ordinary_lines_alone() {
        let line = "Catalyst Link 1.0.0 — ThomasRog";
        assert_eq!(redact(line), line);
    }
}
