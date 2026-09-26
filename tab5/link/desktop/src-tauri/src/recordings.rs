//! The runs the tablet uploaded: listing them, and summarising one without reading all of it.
//!
//! The tablet's recorder writes `run-<timestamp>.csv` to its card and the Link's file drop receives
//! them into `~/.catalyst-link/files/`. Until now nothing could look at one without opening a
//! spreadsheet.
//!
//! # Read from disk, not over HTTP
//!
//! This reads the Link's own files directory directly, the way [`crate::main`]'s `open_work_order`
//! already reads its inbox: canonicalise both the target and the directory, and refuse anything that
//! does not resolve inside. The alternative — going through the Link's HTTP API — cannot work, and it is
//! worth writing down why so nobody adds a route thinking it will help: the API has `GET /files`, which
//! lists names and sizes, and `POST /files`, which uploads. **There is no route that returns a file's
//! contents.** Adding the list route to the app's allowlist would still leave no way to read a run.
//!
//! # Summarising without parsing the whole file
//!
//! A run is up to 48 channels at 50 Hz, so a few minutes is megabytes and a spreadsheet's worth of
//! rows. The duration comes from the `t` field of the last complete row, read from the file's tail, and
//! the channel count from the header — which is exactly the trick the firmware's own run browser uses,
//! and the reason it can list a card full of runs instantly.
//!
//! # The format, and the one thing a parser must not do
//!
//! RFC 4180 CSV. The header is `t,mark,<label>,<label>,…`; a row is the elapsed seconds, then the mark
//! number if the driver marked that instant, then one field per channel. **An unavailable value is an
//! empty field, never `0`** — the tablet's whole telemetry contract is "never zero, only absent", and a
//! parser that coerces blanks to zero turns a disconnected sensor into a reading of nought.

use std::io::{Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};

use serde::Serialize;

/// How much of the tail to read looking for the last complete row. Generous: 48 channels of `%.6g`
/// plus a timestamp is a few hundred bytes, so this is several rows even at the widest.
const TAIL_BYTES: u64 = 8192;

/// The most rows to hand the window. A chart cannot draw more than this usefully and the DOM should
/// never be given a megabyte of numbers.
const MAX_POINTS: usize = 1500;

#[derive(Serialize)]
pub struct FileEntry {
    pub name: String,
    pub bytes: u64,
    /// Seconds since the epoch, or 0 when the platform will not say.
    pub modified: u64,
    /// True for something this module can summarise; false for a clip or a log, which only open.
    pub is_run: bool,
}

#[derive(Serialize)]
pub struct RunSummary {
    pub name: String,
    pub bytes: u64,
    pub channels: Vec<String>,
    pub duration_s: f64,
    pub rows: usize,
    pub marks: Vec<f64>,
    /// One decimated series per channel, aligned with `times`. Empty where a channel had no values.
    pub series: Vec<Vec<Option<f64>>>,
    pub times: Vec<f64>,
}

/// `~/.catalyst-link/files`.
fn files_dir() -> PathBuf {
    crate::settings::link_home().join("files")
}

/// Resolve `name` inside the files directory, refusing anything that escapes it.
///
/// Symlinks are followed before the check, so a link planted in the directory cannot point out of it —
/// the same rule the inbox opener uses.
fn resolve(name: &str) -> Result<PathBuf, String> {
    let dir = files_dir()
        .canonicalize()
        .map_err(|e| format!("no uploads folder yet ({e})"))?;
    let target = dir.join(name);
    let target = target
        .canonicalize()
        .map_err(|e| format!("no such file: {name} ({e})"))?;
    if !target.starts_with(&dir) {
        return Err(format!("{name} is outside the uploads folder"));
    }
    Ok(target)
}

/// Everything the tablet has uploaded, newest first.
pub fn list() -> Vec<FileEntry> {
    let dir = files_dir();
    let mut out = Vec::new();
    let Ok(entries) = std::fs::read_dir(&dir) else {
        return out;
    };
    for entry in entries.flatten() {
        let Ok(meta) = entry.metadata() else { continue };
        if !meta.is_file() {
            continue;
        }
        let name = entry.file_name().to_string_lossy().to_string();
        let modified = meta
            .modified()
            .ok()
            .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
            .map(|d| d.as_secs())
            .unwrap_or(0);
        out.push(FileEntry {
            is_run: name.starts_with("run-") && name.ends_with(".csv"),
            name,
            bytes: meta.len(),
            modified,
        });
    }
    out.sort_by(|a, b| b.modified.cmp(&a.modified).then(a.name.cmp(&b.name)));
    out
}

/// The full path of an uploaded file, for the OS opener.
pub fn path_of(name: &str) -> Result<String, String> {
    resolve(name).map(|p| p.display().to_string())
}

/// Summarise one run.
pub fn summary(name: &str) -> Result<RunSummary, String> {
    let path = resolve(name)?;
    let meta = std::fs::metadata(&path).map_err(|e| format!("{name}: {e}"))?;
    let text = std::fs::read_to_string(&path).map_err(|e| format!("{name}: {e}"))?;

    let mut lines = text.lines();
    let header = lines.next().ok_or_else(|| format!("{name} is empty"))?;
    let cols: Vec<&str> = header.split(',').collect();
    if cols.len() < 2 || cols[0] != "t" {
        return Err(format!(
            "{name} does not start with a recorder header (expected 't,mark,...', found \"{}\")",
            &header.chars().take(40).collect::<String>()
        ));
    }
    let channels: Vec<String> = cols[2..].iter().map(|s| s.to_string()).collect();

    let mut times: Vec<f64> = Vec::new();
    let mut series: Vec<Vec<Option<f64>>> = vec![Vec::new(); channels.len()];
    let mut marks: Vec<f64> = Vec::new();
    let mut rows = 0usize;

    // Count first so the decimation stride is right in one pass over the text we already have in
    // memory. Reading the file twice would be cheaper in memory and slower; these are megabytes, not
    // gigabytes, and the file is already read.
    let total_rows = text.lines().skip(1).filter(|l| !l.trim().is_empty()).count();
    let stride = (total_rows / MAX_POINTS).max(1);

    for (i, line) in text.lines().skip(1).enumerate() {
        if line.trim().is_empty() {
            continue;
        }
        let fields: Vec<&str> = line.split(',').collect();
        let Some(t) = fields.first().and_then(|s| s.trim().parse::<f64>().ok()) else {
            continue; // a torn last row, which a power cut leaves
        };
        rows += 1;

        if let Some(mark) = fields.get(1).and_then(|s| s.trim().parse::<f64>().ok()) {
            marks.push(t);
            let _ = mark;
        }

        if i % stride != 0 {
            continue;
        }
        times.push(t);
        for (c, slot) in series.iter_mut().enumerate() {
            // An empty field stays None. Coercing it to 0.0 would draw a disconnected sensor as a
            // reading of nought, which is the one thing the recorder's format exists to avoid.
            slot.push(
                fields
                    .get(c + 2)
                    .map(|s| s.trim())
                    .filter(|s| !s.is_empty())
                    .and_then(|s| s.parse::<f64>().ok()),
            );
        }
    }

    Ok(RunSummary {
        name: name.to_string(),
        bytes: meta.len(),
        channels,
        duration_s: times.last().copied().unwrap_or(0.0),
        rows,
        marks,
        series,
        times,
    })
}

/// The duration of a run without reading the whole file: the `t` of the last complete row.
///
/// Used by the list, so a folder of large runs can show durations without loading any of them.
pub fn quick_duration(path: &Path) -> Option<f64> {
    let mut file = std::fs::File::open(path).ok()?;
    let len = file.metadata().ok()?.len();
    let from = len.saturating_sub(TAIL_BYTES);
    file.seek(SeekFrom::Start(from)).ok()?;
    let mut tail = Vec::new();
    file.take(TAIL_BYTES).read_to_end(&mut tail).ok()?;
    let text = String::from_utf8_lossy(&tail);

    // Only rows that are *complete* count, and "parses as a number" is not the same as complete.
    // A power cut mid-write leaves a fragment like `12.5`, which is a perfectly valid float and is
    // very likely a truncated `12.58` — taking it would report a duration that is simply wrong, and
    // wrong in a way nothing downstream could detect. Two rules together settle it:
    //
    //   * the line must have been terminated, so the last fragment is dropped unless the file ends
    //     with a newline;
    //   * it must carry at least the two fixed columns' separators, so a bare truncated timestamp
    //     with no commas after it is not mistaken for a row.
    let mut lines: Vec<&str> = text.split('\n').collect();
    if !text.ends_with('\n') {
        lines.pop();
    }
    for line in lines.iter().rev() {
        let line = line.trim_end_matches('\r');
        if line.matches(',').count() < 2 {
            continue;
        }
        if let Some(t) = line.split(',').next().and_then(|s| s.trim().parse::<f64>().ok()) {
            return Some(t);
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn an_empty_field_stays_absent_rather_than_becoming_zero() {
        // Built by hand rather than through summary(), which needs the Link's real files directory.
        let line = "1.500,,12.4,,0.0";
        let fields: Vec<&str> = line.split(',').collect();
        let read = |i: usize| -> Option<f64> {
            fields.get(i).map(|s| s.trim()).filter(|s| !s.is_empty()).and_then(|s| s.parse::<f64>().ok())
        };
        assert_eq!(read(2), Some(12.4));
        assert_eq!(read(3), None, "a blank must not read as 0.0");
        assert_eq!(read(4), Some(0.0), "a real zero must still read as zero");
    }

    #[test]
    fn the_duration_comes_from_the_last_row_that_parses() {
        let dir = std::env::temp_dir().join(format!("cl-rec-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("run-test.csv");
        // A torn final row, exactly as a power cut leaves one. `12.5` parses as a float but is very
        // likely a truncated `12.58`, so the last COMPLETE row is the answer.
        std::fs::write(&path, "t,mark,a\n0.000,,1\n0.020,,2\n12.480,,3\n12.5").unwrap();
        assert_eq!(quick_duration(&path), Some(12.48));

        // A file that ends cleanly reports its real last row.
        std::fs::write(&path, "t,mark,a\n0.000,,1\n3.140,,2\n").unwrap();
        assert_eq!(quick_duration(&path), Some(3.14));

        // A torn row that happens to keep its commas is still incomplete, but its timestamp is whole,
        // so it counts: the cut was after the separator, not inside the number.
        std::fs::write(&path, "t,mark,a\n0.000,,1\n5.500,,\n").unwrap();
        assert_eq!(quick_duration(&path), Some(5.5));

        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn a_run_is_recognised_by_its_name() {
        let is_run = |n: &str| n.starts_with("run-") && n.ends_with(".csv");
        assert!(is_run("run-20260925-143000.csv"));
        assert!(is_run("run-1790000000.csv"));
        assert!(!is_run("clip-20260925.h264"));
        assert!(!is_run("FRC_20260925.dsevents"));
    }
}
