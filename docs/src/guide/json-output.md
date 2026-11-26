# Structured JSON Output (`--json`)

When the `--json` flag is set on `convert` or `convert access-file`
(e.g. `sipi convert input.jpg out.jp2 --json`), sipi emits a single JSON
document to `stdout`, on success and on failure. The document mirrors
the internal `ImageContext` that sipi otherwise sends to Sentry.

`query` and `compare` also take `--json`: they keep their text output
on success, and emit the JSON error document only when they fail.
`verify` takes `--json` too, but only routes its log lines to `stderr`;
it emits no JSON document.

This page is the reference for the schema, the emission rules, and the
exit-code / stream contract.

## Contract

- `stdout` contains **exactly one** JSON document terminated by a
  trailing newline. Nothing precedes the opening `{` and nothing
  follows the matching `}`.
- `stderr` carries every log line (info, warning, and error). This is a
  change from the default CLI log routing, where info/warn go to
  `stdout`.
- Exit codes are unchanged: `0` on success, non-zero on failure.
- The flag is CLI-only: passing `--json` together with `--config`
  (server mode) is silently ignored. Server-mode errors continue to go
  through Sentry and HTTP responses.
- `--json` attaches via the `output_opts` group to `convert`,
  `convert access-file`, `query`, `compare`, and all `verify`
  subcommands (per the D5 option-availability matrix).
- Every JSON document carries `correlation_id`, and every error document
  carries `error_code` (see [Error codes](#error-codes) and
  [Correlation ID](#correlation-id)).
- The schema is **not versioned** today — there is a single consumer
  (local debugging / ad-hoc integrations) and the cost of adding a
  `schema_version` field exceeds its value while there is only one
  version. Future changes are additive and documented on this page.

## Success payload

Emitted when the conversion completes successfully.

```json
{
  "status": "ok",
  "mode": "cli",
  "correlation_id": "4f9c2a7e0b1d4c38a6e5f0b2c9d81e47",
  "input_file": "/path/to/input.jpg",
  "output_file": "/tmp/out.jp2",
  "output_format": "jpx",
  "file_size_bytes": 26688,
  "image": {
    "width": 404,
    "height": 201,
    "channels": 3,
    "bps": 8,
    "colorspace": "RGB",
    "icc_profile_type": "sRGB",
    "orientation": "TOPLEFT"
  }
}
```

| Field | Type | Description |
|---|---|---|
| `status` | string | Always `"ok"` on the success payload. |
| `mode` | string | Always `"cli"`. |
| `correlation_id` | string | The run's correlation ID (see [Correlation ID](#correlation-id)). |
| `input_file` | string | Absolute path to the source image. |
| `output_file` | string | Absolute path to the produced output file. |
| `output_format` | string | Output format (`"jpx"`, `"tif"`, `"jpg"`, `"png"`). |
| `file_size_bytes` | integer | Size of the source image in bytes. |
| `image` | object | Decoded image properties (see below). |
| `image.width` | integer | Width in pixels. |
| `image.height` | integer | Height in pixels. |
| `image.channels` | integer | Samples per pixel (e.g. 3 for RGB, 4 for CMYK/RGBA). |
| `image.bps` | integer | Bits per sample (8 or 16). |
| `image.colorspace` | string | Photometric interpretation: `MINISBLACK`, `MINISWHITE`, `RGB`, `YCBCR`, `SEPARATED`, etc. |
| `image.icc_profile_type` | string | Well-known profile name (`sRGB`, `AdobeRGB`, `CMYK (USWebCoatedSWOP)`, `Gray D50`, …) or `"unknown/embedded"` for arbitrary ICC buffers. |
| `image.orientation` | string | TIFF orientation tag value (e.g. `TOPLEFT`). |

## Error payload

Emitted when any phase of the run fails.

```json
{
  "status": "error",
  "mode": "cli",
  "correlation_id": "e2e-123",
  "phase": "read",
  "error_code": "metadata_parse_failed",
  "error_message": "... /path/to/input.jpg: IPTC block does not parse: Exiv2 rejected the block (decode code 5)",
  "input_file": "/path/to/input.jpg",
  "output_file": "/tmp/out.jp2",
  "output_format": "jpx",
  "file_size_bytes": 26724,
  "image": {
    "width": 0,
    "height": 0,
    "channels": 0,
    "bps": 0,
    "colorspace": "",
    "icc_profile_type": "",
    "orientation": ""
  }
}
```

| Field | Type | Description |
|---|---|---|
| `status` | string | Always `"error"` on the error payload. |
| `correlation_id` | string | The run's correlation ID (see [Correlation ID](#correlation-id)). |
| `phase` | string | `"cli_args"`, `"read"`, `"convert"`, `"write"` (in order of when they can fire), `"unknown"`, or `"internal"`. |
| `error_code` | string | What failed (see [Error codes](#error-codes)). Only on the error payload. |
| `error_message` | string | The full diagnostic text of the failure (or the parameter validation failure). |
| `image` | object, optional | Populated with whatever `ImageContext` captured before the failure point; **omitted entirely** when `phase == "cli_args"` because no image was loaded. |

### Phase semantics

| Phase | Meaning |
|---|---|
| `cli_args` | Failure during CLI argument validation — invalid `--size` / `--scale` syntax, unknown output extension, a `compare` input that does not exist. Fires before any image is loaded; the `image` object is omitted. |
| `read` | Failure while decoding the input file (corrupt JPEG, unsupported TIFF variant, a metadata block that does not parse, I/O error), or the decode deadline. Only decode errors of the input file. The `image` object reflects any properties parsed from the header before the failure. |
| `convert` | Failure after the decode — the conversion to 8 bits and sRGB for a JPEG output, orientation, ICC conversion, rotation, watermarking, the `compare` difference image. The `image` object reflects the fully-decoded input image. |
| `write` | Failure while encoding or writing the output file (format-specific encoder errors, disk full). |
| `unknown` | An exception that no phase caught. |
| `internal` | The report itself could not be built on a successful run. |

If the error message is not valid UTF-8 (for example, a Latin-1 file
path), the document keeps `phase` and `error_code`, and `error_message`
is `"SipiReport: json_pack failed"`; the full message is on `stderr`.

## Error codes

`error_code` names what failed. The names are a contract with callers
such as dsp-api; a new code is added to this table.

| `error_code` | Meaning |
|---|---|
| `decode_failed` | The decoder could not decode the image data. |
| `unsupported_format` | No format handler recognised the file. |
| `malformed_input` | The file's structure is invalid (bad header fields, truncated boxes, dimensions out of range). |
| `metadata_parse_failed` | An embedded metadata block (EXIF, XMP, IPTC, ICC) does not parse. Such a file is rejected, not served without the block. |
| `write_failed` | The encoder could not write the output. |
| `shape_probe_failed` | The image's dimensions could not be read. |
| `invalid_request_parameter` | A request parameter is out of range. |
| `client_abort` | The client went away (server mode only). |
| `cli_args` | A CLI argument is invalid (phase `cli_args`). |
| `internal` | A failure without an `ErrorCode`: a caught exception, the decode deadline, or an unknown error. |

### Rule for callers: 4xx or 5xx

In phase `read`, the codes `decode_failed`, `unsupported_format`,
`malformed_input` and `metadata_parse_failed` mean that the input file
is invalid:

- If the input file is the user's upload, answer with a 4xx and the
  message: the user must fix the file.
- If sipi created the input file (for example, `sipi query` on the JPX
  that `sipi convert` just wrote), answer with a 5xx: a file sipi wrote
  must read back.

Every other code, and every code in another phase, is a 5xx.

## Correlation ID

Every JSON document carries `correlation_id`, the ID that finds the
run's log lines. Show it to the user as the error ID.

- A caller passes its own ID in the environment variable
  `SIPI_CORRELATION_ID`, so its logs and sipi's logs share one ID. The
  value must be 1 to 128 characters of `A-Z a-z 0-9 . _ : -`.
- Without the variable, or with an invalid value, sipi generates 16
  random bytes in lowercase hex (32 characters). An invalid value is
  replaced with a warning on `stderr`.
- The first line of each CLI log call starts with `[<id>] `, so one
  search finds every log line of the run. Server-mode logs are
  unchanged.
- The Sentry tag `correlation_id` carries the same ID.
- The C++-only `//src/cli:sipi` binary (used by the approval tests)
  sets no ID: its documents carry `"correlation_id": ""` and its log
  lines have no prefix.

`sipi query` reads only the header and the embedded metadata of its input and
decodes no pixels. A failure in its `read` phase is a header or metadata
failure; a file whose pixel data is corrupt but whose header reads passes
`query`.

### Reserved fields

`request_uri` is part of the internal `ImageContext` struct (it is populated
during HTTP request handling) but is **intentionally omitted** from the CLI
`--json` output. It is reserved for potential future server-side use of this
emitter.

## Worked examples

### Success — convert a JPEG to JPEG 2000

```console
$ sipi convert input.jpg out.jp2 --json 2>/dev/null | jq
{
  "status": "ok",
  "mode": "cli",
  "correlation_id": "4f9c2a7e0b1d4c38a6e5f0b2c9d81e47",
  "input_file": "input.jpg",
  "output_file": "out.jp2",
  "output_format": "jpx",
  "file_size_bytes": 26688,
  "image": {
    "width": 404,
    "height": 201,
    "channels": 3,
    "bps": 8,
    "colorspace": "RGB",
    "icc_profile_type": "sRGB",
    "orientation": "TOPLEFT"
  }
}
```

### `cli_args` error — invalid `--size`

```console
$ SIPI_CORRELATION_ID=req-42 sipi convert input.jpg out.jp2 --size bogus --json 2>/dev/null | jq
{
  "status": "error",
  "mode": "cli",
  "correlation_id": "req-42",
  "phase": "cli_args",
  "error_code": "cli_args",
  "error_message": "Error in size parameter: ...",
  "input_file": "",
  "output_file": "",
  "output_format": "",
  "file_size_bytes": 0
}
```

Note the absence of `image`.

### Shell pipeline

The structured output is shell-pipeable because log chatter is kept out
of `stdout`:

```bash
if sipi convert input.jpg out.jp2 --json 2>sipi.log | jq -e '.status == "ok"' >/dev/null; then
  echo "conversion succeeded"
else
  echo "conversion failed — see sipi.log"
  cat sipi.log
fi
```
