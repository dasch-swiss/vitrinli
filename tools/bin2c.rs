// bin2c — embed a binary file as a C array, replacing `xxd -i`.
//
// Usage: bin2c <input> <output> <symbol>
//
// Emits the wire format the SIPI codecs consume:
//   unsigned char <symbol>[] = { 0x.., ... };
//   unsigned int  <symbol>_len = <count>;
//
// Built by the hermetic Rust toolchain in exec config so the ICC and
// magic-database genrules run on lean RBE images that ship no xxd. Output is
// uniform across macOS, Linux, and RBE — unlike xxd, whose Toybox and GNU
// flavours format zero bytes and the length line differently.
use std::env;
use std::fs::{self, File};
use std::io::{self, BufWriter, Write};
use std::process::ExitCode;

// An LFS-tracked input in a checkout without `git lfs pull` is pointer text;
// embedding it would ship a binary with unusable assets.
const LFS_POINTER_PREFIX: &[u8] = b"version https://git-lfs.github.com/spec/";

fn run(input: &str, output: &str, symbol: &str) -> io::Result<()> {
    let bytes = fs::read(input)?;
    if bytes.starts_with(LFS_POINTER_PREFIX) {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "input is a Git LFS pointer, not the file; a file that bin2c compiles in must be a plain git object (exempt it from LFS in .gitattributes)",
        ));
    }
    let mut out = BufWriter::new(File::create(output)?);
    writeln!(out, "unsigned char {symbol}[] = {{")?;
    for (i, b) in bytes.iter().enumerate() {
        if i % 12 == 0 {
            write!(out, "  ")?;
        }
        write!(out, "0x{b:02x}")?;
        if i + 1 < bytes.len() {
            write!(out, "{}", if i % 12 == 11 { ",\n" } else { ", " })?;
        }
    }
    writeln!(out, "\n}};\nunsigned int {symbol}_len = {};", bytes.len())?;
    out.flush()
}

fn main() -> ExitCode {
    let args: Vec<String> = env::args().collect();
    let [_, input, output, symbol] = args.as_slice() else {
        eprintln!("usage: bin2c <input> <output> <symbol>");
        return ExitCode::FAILURE;
    };
    if let Err(e) = run(input, output, symbol) {
        eprintln!("bin2c: {input} -> {output}: {e}");
        return ExitCode::FAILURE;
    }
    ExitCode::SUCCESS
}
