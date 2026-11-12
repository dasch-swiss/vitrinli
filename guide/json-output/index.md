# Structured JSON Output (`--json`)

When the `--json` flag is set on a `query` / `compare` / `verify` subcommand invocation (e.g. `sipi query --json <input>` or `sipi verify service-file --json <input>`) sipi emits a single JSON document to `stdout` instead of human-readable text. The document mirrors the internal `ImageContext` that sipi otherwise sends to Sentry, so every CLI run — success or failure — yields the same structured payload a Sentry event would.

This page is the reference for the schema, the emission rules, and the exit-code / stream contract.

## Contract

- `stdout` contains **exactly one** JSON document terminated by a trailing newline. Nothing precedes the opening `{` and nothing follows the matching `}`.
- `stderr` carries every log line (info, warning, and error). This is a change from the default CLI log routing, where info/warn go to `stdout`.
- Exit codes are unchanged: `0` on success, non-zero on failure.
- The flag is CLI-only: passing `--json` together with `--config` (server mode) is silently ignored. Server-mode errors continue to go through Sentry and HTTP responses.
- `--json` attaches via the `output_opts` group to the `query`, `compare`, and all `verify` subcommands (per the D5 option-availability matrix). It is not exposed on `convert` flows.
- The schema is **not versioned** today — there is a single consumer (local debugging / ad-hoc integrations) and the cost of adding a `schema_version` field exceeds its value while there is only one version. Future changes are additive and documented on this page.

## Success payload

Emitted when the conversion completes successfully.

```
{
  "status": "ok",
  "mode": "cli",
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

| Field                    | Type    | Description                                                                                                                              |
| ------------------------ | ------- | ---------------------------------------------------------------------------------------------------------------------------------------- |
| `status`                 | string  | Always `"ok"` on the success payload.                                                                                                    |
| `mode`                   | string  | Always `"cli"`.                                                                                                                          |
| `input_file`             | string  | Absolute path to the source image.                                                                                                       |
| `output_file`            | string  | Absolute path to the produced output file.                                                                                               |
| `output_format`          | string  | Output format (`"jpx"`, `"tif"`, `"jpg"`, `"png"`).                                                                                      |
| `file_size_bytes`        | integer | Size of the source image in bytes.                                                                                                       |
| `image`                  | object  | Decoded image properties (see below).                                                                                                    |
| `image.width`            | integer | Width in pixels.                                                                                                                         |
| `image.height`           | integer | Height in pixels.                                                                                                                        |
| `image.channels`         | integer | Samples per pixel (e.g. 3 for RGB, 4 for CMYK/RGBA).                                                                                     |
| `image.bps`              | integer | Bits per sample (8 or 16).                                                                                                               |
| `image.colorspace`       | string  | Photometric interpretation: `MINISBLACK`, `MINISWHITE`, `RGB`, `YCBCR`, `SEPARATED`, etc.                                                |
| `image.icc_profile_type` | string  | Well-known profile name (`sRGB`, `AdobeRGB`, `CMYK (USWebCoatedSWOP)`, `Gray D50`, …) or `"unknown/embedded"` for arbitrary ICC buffers. |
| `image.orientation`      | string  | TIFF orientation tag value (e.g. `TOPLEFT`).                                                                                             |

## Error payload

Emitted when any phase of the conversion fails.

```
{
  "status": "error",
  "mode": "cli",
  "phase": "read",
  "error_message": "Images with 1 bit/sample not supported in file ...",
  "input_file": "/path/to/bilevel.tif",
  "output_file": "/tmp/out.jp2",
  "output_format": "jpx",
  "file_size_bytes": 4768164,
  "image": {
    "width": 8040,
    "height": 9624,
    "channels": 1,
    "bps": 1,
    "colorspace": "MINISWHITE",
    "icc_profile_type": "",
    "orientation": "TOPLEFT"
  }
}
```

| Field           | Type             | Description                                                                                                                                            |
| --------------- | ---------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `status`        | string           | Always `"error"` on the error payload.                                                                                                                 |
| `phase`         | string           | `"cli_args"`, `"read"`, `"convert"`, or `"write"` (in order of when they can fire).                                                                    |
| `error_message` | string           | The message from the thrown exception (or the parameter validation failure).                                                                           |
| `image`         | object, optional | Populated with whatever `ImageContext` captured before the failure point; **omitted entirely** when `phase == "cli_args"` because no image was loaded. |

### Phase semantics

| Phase      | Meaning                                                                                                                                                                          |
| ---------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `cli_args` | Failure during CLI argument validation — invalid `--size` / `--scale` syntax, unknown output extension. Fires before any image is loaded; the `image` object is omitted.         |
| `read`     | Failure while decoding the input file (corrupt JPEG, unsupported TIFF variant, I/O error). The `image` object reflects any properties parsed from the header before the failure. |
| `convert`  | Failure during the intermediate processing pipeline — orientation, ICC conversion, rotation, watermarking. The `image` object reflects the fully-decoded input image.            |
| `write`    | Failure while encoding or writing the output file (format-specific encoder errors, disk full).                                                                                   |

### Reserved fields

`request_uri` is part of the internal `ImageContext` struct (it is populated during HTTP request handling) but is **intentionally omitted** from the CLI `--json` output. It is reserved for potential future server-side use of this emitter.

## Worked examples

### Success — convert a JPEG to JPEG 2000

```
$ sipi --json --file input.jpg out.jp2 2>/dev/null | jq
{
  "status": "ok",
  "mode": "cli",
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

### `cli_args` error — invalid `--scale`

```
$ sipi --json --file input.jpg out.jp2 --scale bogus 2>/dev/null | jq
{
  "status": "error",
  "mode": "cli",
  "phase": "cli_args",
  "error_message": "Error in scale parameter: ...",
  "input_file": "",
  "output_file": "",
  "output_format": "",
  "file_size_bytes": 0
}
```

Note the absence of `image`.

### Shell pipeline

The structured output is shell-pipeable because log chatter is kept out of `stdout`:

```
if sipi --json --file input.jpg out.jp2 2>sipi.log | jq -e '.status == "ok"' >/dev/null; then
  echo "conversion succeeded"
else
  echo "conversion failed — see sipi.log"
  cat sipi.log
fi
```
