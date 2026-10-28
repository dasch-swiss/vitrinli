# Basic Information and Reference

This section provides the basic information to use Vitrinli as a high performance, versatile media server implementing the
[IIIF](https://iiif.io) standards that can be used in many different settings, from a small standalone server providing
basic metadata to the deployment in a complex environment. For more information about the IIIF standard see
[https://iiif.io](https://iiif.io). The basic idea is that an image or rectangular region of an image can be downloaded
(e.g. to the browser) with a given width and height, rotation, image quality and format. All parameters are provided
with the IIIF conformant URL that has the following form:

```http(s)://{server}/{prefix}/{identifier}/{region}/{size}/{rotation}/{quality}.{format}```

The parts do have the following meaning:

- `{server}`: The DNS name of the server, eg. `iiif.dasch.swiss`. The server may include a portnumber,
  eg. `iiif2.dasch.swiss:8080`.
- `{prefix}`: A path (that may include `/`'s) to organize the assets. Usually the prefix reflect the internal
  directory or folder hierarchy. However this can be overridden using special features of Vitrinli (see pre-flight-script
  and sipi configuration file).
- `{identifier}`: The identifier of the requested image. By default, it is the filename and its extension.
- `{region}`: a region of interest that should be displayed. `full` indicates that the whole image is being requested.
  For more details see [IIIF regions](https://iiif.io/api/image/3.0/#41-region)
- `{size}`: The size of the displayed image (part). `max` indicates the the "natural" maximal resolution should be used.
  For more details see [IIIF size](https://iiif.io/api/image/3.0/#42-size)
- `{rotations}`: The image can be rotated and mirrored before being transmitted to the client. Vitrinli allows for
  arbitrary rotations. The rotation is a non-negative decimal — digits and `.` only — per
  [IIIF Image API 3.0 §5.1.1](https://iiif.io/api/image/3.0/#511-rotation). The value `0` indicates no rotation.
  Prefix with `!` to mirror first, then rotate. See [IIIF rotation](https://iiif.io/api/image/3.0/#43-rotation).
- `{quality}`: The quality parameter determines whether the image is delivered in color, grayscale or black and white.
  Valid values are:
  - `default`: the "natural" quality of the original image
  - `color`: A color representation
  - `gray`: A gray value representation
  - `bitonal`: A bitonal representation
  
  All quality values are supported by Vitrinli
- `{format}`: The file format that should be delivered. The IIIF endpoint supports the following formats,
  independent of the image's storage format:
  - `jpg`: JPEG. The IIIF spec does not allow per-request selection of the JPEG compression ratio; a server-wide
    rate is set in the configuration file.
  - `tif`: TIFF.
  - `png`: PNG.
  - `jp2` (also `jpx`): JPEG 2000.

  `info.json` advertises `extraFormats: ["tif", "jp2"]`; JPEG and PNG are the IIIF default set.


## The Vitrinli executable (`sipi`)
The Vitrinli executable is a statically linked program that can be started as
- _command line tool_ to perform image operations, mainly format conversions
- _as server deamon_ that provides IIIF conforming media server

### Using Vitrinli as Command Line Tool

The Vitrinli command line mode can be used for the following tasks:

#### Format Conversions:

```bash
/path/to/sipi convert infile outfile [options]
```

For DSP preservation-pipeline workflows, use the pipeline-stage forms:
`sipi convert access-file ...`, `sipi convert service-file ...`,
or `sipi convert preservation-file ...` (the last is a stub awaiting
ADR-0012).

#### Print Information about File and Metadata:

```bash
/path/to/sipi query infile
```

#### Compare two Images pixelwise
The images may have different formats: if the have exactely the same pixels, they are considered identical). Metadata
is ignored for comparison:

```bash
/path/to/sipi compare file1 file2
```

#### Health Check
Probe a running Vitrinli server's `/health` endpoint and exit `0` (healthy) or `1`
(unhealthy). Used as the container/orchestrator liveness command:

```bash
/path/to/sipi health [--port <n>]   # default port 1024
```

See [Running Vitrinli](running.md) and [Health Endpoint](../operation/health-endpoint.md)
for details.

#### General Options for the Command Line Use 
In command line mode, Vitrinli supports the following options:

- `-h`, `--help`: Display a short help with all options available
- `-F <fmt>`, `--format <fmt>`: The format of the output file. Valid are `jpx`, `jp2`, `jpg`, `png`, `tif`, `webp`, and `gif`.
- `-I <profile>`, `--icc <profile>`: Convert the outfile to the given ICC color profile. Supported profiles are `sRGB`,
  `AdobeRGB` and `GRAY`.
- `-q <num>`, `--quality <num>`: Only used for the JPEG format. Ignored for all other formats. Its a number between 1 and
  100, where 1 is equivalent to the highest compression ratio and lowest quality, 100 to the lowest compression ration
  and highest quality of the output image.
- `-n <num>`, `--pagenum <num>`: Only for input files in multi-page PDF format: sets the page that should be converted.
  Ignored for all other input file formats.
- `-r <x> <y> <nx> <ny>`, `--region <x> <y> <nx> <ny>`: Selects a region of interest that should be converted. Needs
  4 integer values: `left_upper_corner_X`, `left_upper_corner_Y`, `width`, `height`.
- `-s <iiif-size>`, `--size <iif-size>`: The size of the resulting image. The option requires a string parameter
  formatted according to the size-syntax of IIIF [see IIIF-Size](https://iiif.io/api/image/3.0/#42-size). Not giving
  this parameters results in having the maximalsize (as the value `"max"`would give).
- `-s <num>`, `--scale <num>`: Scaling the image size by the given number (interpreted as percentage). Percentage must
  be given as integer value. It may be bigger than 100 to upscale an image.
- `-R <num>`, `--reduce <num>`: Reduce the size of the image by the given factor. Thus `-R 2`would resize the image to
  half of the original size. Using `--reduce` is usually much faster than using `--scale`, e.g. `--reduce 2` is faster
  than `--scale 50`.
- `-m <val>`, `--mirror <val>`: Takes either `horizontal` or `vertical`as parameter to mirror the image appropriately.
- `-o <angle>`, `--rotate <angle>`: Rotates the image by the given angle. The angle must be a floating point (or
  integer) value between `0.0`and `w60.0`.
- `-k`, `--skipmeta`: Strip all metadata from inputfile.
- `-w <filepath>`, `--watermark <filepath>`: Overlays a watermark to the output image. <filepath> must be a single
  channel, gray valued TIFF. That is, the TIFF file must have the following tag values: SAMPLESPERPIXEL = 1,
  BITSPERSAMPLE = 8, PHOTOMETRIC = PHOTOMETRIC_MINISBLACK.
- `--json`: Emit a single structured JSON document to stdout instead of human-readable output. The document mirrors
  the internal `ImageContext` that is otherwise sent to Sentry and includes the input/output paths, the decoded image
  properties, and — on failure — a `phase` (`cli_args` | `read` | `convert` | `write`) with the `error_message`.
  Useful for programmatic consumers and for debugging when no Sentry DSN is configured. Stderr carries any log output
  so stdout stays reserved for the single JSON document. Attached only to the `query`, `compare`, and `verify`
  subcommands per the D5 option-availability matrix; convert flows do not expose `--json`.
  See [`json-output.md`](json-output.md) for the full schema and worked examples. Example:
  ```bash
  sipi query --json input.jpg | jq '.image.bps'
  ```
  
#### JPEG2000 Specific Options
Usually, the Vitrinli command line tool is used to create JPEG2000 images suitable for a IIIF repository. Vitrinli supports
the following JPEG2000 specific options. For a in detail description of these options consult the kakadu documentation!

- `--Sprofile <profile>`: The following JPEG2000 profiles are supported: `PROFILE0`, `PROFILE1`, `PROFILE2`, `PART2`,
  `CINEMA2K`, `CINEMA4K`, `BROADCAST`, `CINEMA2S`, `CINEMA4S`, `CINEMASS`, `IMF`. Default: `PART2`.
- `--rates <string>`: One or more bit-rates (see kdu_compress help!). A value "-1" may be used in place of the first
  bit-rate in the list to indicate that the final quality layer should include all compressed bits.
- `--Clayers <num>`:Number of quality layers. Default: 8.
- `--Clevels <num>`: Number of wavelet decomposition levels, or stages. Default: 8.
- `--Corder <val>`: Progression order. The four character identifiers have the following interpretation:
  L=layer; R=resolution; C=component; P=position. The first character in the identifier refers to the index which
  progresses most slowly, while the last refers to the index which progresses most quickly.
  Thus must be one of `LRCP`, `RLCP`, `RPCL`, `PCRL`, `CPRL`, Default: `RPCL`.
- `--Stiles <string>`: Tiles dimensions `"{tx,ty}"`. Default: `"{256,256}"`.
- `--Cprecincts <string>`: Precinct dimensions `"{px,py}"` (must be powers of 2). Default: `"{256,256}"`.
- `--Cblk <string>`: Nominal code-block dimensions `"{dx,dy}"`(must be powers of 2, no less than 4 and no greater than 1024,
  whose product may not exceed 4096). Default: `"{64,64}"`.
- `--Cuse_sop <val>`: Include SOP markers (i.e., resync markers). Default: yes.

### Using Vitrinli as IIIF Media Server
In order to use Vitrinli as IIIF media server, some setup work has to be done. The *configuration* of Vitrinli can be done
using a configuration file (written in Lua or, equivalently, in TOML — see
[Configuration file format](running.md#configuration-file-format-lua-or-toml)) and/or using environment variables,
and/or command line options.

The priority is as follows: *`configuration file parameters` are overwritten by `environment variables` are overwritten
by `command line options`*.

The Vitrinli server requires a few directories to be setup and listed in the configuration file. Then the Vitrinli server is
launched as follows:

```bash
/path/to/sipi server --config /path/to/config-file.lua
```

#### Vitrinli specific extensions to IIIF
Vitrinli implements some backwards compatible, non-standard extensions to the IIIF Image API:

##### Access to a raw files
Sometimes it may be usefull to store non-image files such as XML-sidecars, manifests as JSON or complete PDF's, etc. in
the same environment as the images. For this reason Vitrinli supports an extension of the IIIF API:

    http(s)://{server}/{prefix}/{identifier}/file
    
The `/file`-path at the end of the URL makes Vitrinli to send the file as it is. Thus, for example a manifest file could
be accessed by

    https://iiif.my.server/images/myimage.json/file
    
This works also for PDF's. The URL

    https://iiif.my.server/images/mydocument.pdf/file
    
will download the PDF in toto to be opened by an external viewer or the webapplication.

It is possible to use the IIIF-`info.json` syntax also on non-image files. In this case the `info.json` has the
following format:

```json
{
   "@context": "http://sipi.io/api/file/3/context.json",
   "id": "http://localhost:1024/images/test.csv",
   "mimeType": "text/comma-separated-values",
   "fileSize": 327
}
```

#### Setup of Vitrinli Directories
Vitrinli needs the following directories and files setup and accessible (the real names of the directories must be indicated in the
configuration file). The following configuration parameters are in the `sipi`-table of the configuration script:

- <a name="imgroot"></a>`imgroot=path`: This is the top-directory of the media file repository. Vitrinli should at least have read access to it. If
  Vitrinli is used to upload and convert files, it must also have write access. The path may be given as absolute path or
  as relative path.  
  *Cmdline option: `--imgroot`*  
  *Environment variable: `SIPI_IMGROOT`*  
  *Default: `./images`*
  
- <a name="scriptinit"></a>`initscript=path/to/init.lua`: Vitrinli needs a minmal set of LUA functions that can be adapted to the local installation.
  These mandatory functions are definied in a init-script (usually it can be found in the config directory where also
  the configuration file is located).  
  *Cmdline option: `--initscript`*  
  *Environment variable: `SIPI_INITSCRIPT`*  
  *Default: `./config/sipi.init.lua`*
  
- <a name="tmpdir"></a>`tmpdir=path`: For the support of multipart POST Vitrinli requires read/write access to a directory to save temporary
  files.  
  *Cmdline option: `--tmpdir`*  
  *Environment variable: `SIPI_IMGROOT`*  
  *Default: `./tmp`*
  
- <a name="scriptdir"></a>`scriptdir=path`: Path to the directory where the LUA-scripts for the routes (e.g. RESTful services) can be found.  
  *Cmdline option: `--scriptdir`*  
  *Environment variable: `SIPI_SCRIPTDIR`*  
  *Default: `./scripts`*
  
- <a name="cachedir"></a>`cachedir=path`: Vitrinli may optionally use a cache directory to store converted image in order to avoid computationally
  intensive conversions if a specific variant is requested several times. Vitrinli starts with a warning if the cache
  directory is defined but not existing.  
  *Cmdline option: `--cachedir`*  
  *Environment variable: `SIPI_CACHEDIR`*  
  *Default: `./cache`*
  
In addition, Vitrinli can act as a webserver that offers image upload and conversion as web service. In order to use this
feature, a server directory has to be defined. This definition ist in the `fileserver`-table of the configuration file:

- <a name="docroot"></a>`docroot=path`: Path to the document root of the Vitrinli web server.  
  *Cmdline option: `--docroot`*  
  *Environment variable: `SIPI_DOCROOT`*  
  *Default: `./server`*
  
#### Vitrinli Configuration Parameters
The following configuration parameters are used by the Vitrinli server:

- <a name="portnum"></a>`port=portnum`: Portnumber Vitrinli should listen on for incoming HTTP requests.  
  *Cmdline option: `--serverport`*  
  *Environment variable: `SIPI_SERVERPORT`*  
  *Default: `80`*
  
- <a name="sslport"></a>`ssl_port=portnum`: Portnumber Vitrinli should listen on for incoming SHTTP requests (using SSL).  
  *Cmdline option: `--sslport`*  
  *Environment variable: `SIPI_SSLPORT`*  
  *Default: `443`*
  
- <a name="prefixaspath"></a>`prefix_as_path=bool`: If `true`, the prefix is used as path within the image root directory. If false, the prefix
  is ignored and it is assumed that all images are directly located in the image root.  
  *Cmdline option: `--pathprefix`*  
  *Environment variable: `SIPI_PATHPREFIX`*  
  *Default: `false`*
  
- <a name="jwt-secret"></a>`jwt_secret=string`: Shared secret to encode web tokens. Required for
  any Lua-configured deployment, at least 32 bytes; there is no default, and
  the server refuses to start without one.  
  *Cmdline option: `--jwtkey`*  
  *Environment variable: `SIPI_JWTKEY`*  
  *Default: none — must be supplied*

- <a name="maxpostsize"></a>`max_post_size=amount`: Maximal size a file upload may have. The amount has the
  form "<number><type>" where `number` is an integer value and `type`an "M" for Megabytes, "G" for Gigabytes
  and "" (empty) for bytes.  
  *Cmdline option: `--maxpost`*  
  *Environment variable: `SIPI_MAXPOSTSIZE`*  
  *Default: `300M`*
  
Request queuing (how many requests may wait for a worker, and how long) is not a
config-file key. It is controlled by the `--max-waiting` / `SIPI_MAX_WAITING` and
`--queue-timeout` / `SIPI_QUEUE_TIMEOUT` CLI flags — see
[Running Vitrinli](running.md#command-line-options).

- <a name="jpegquality">`jpeg_quality=num`: Compression parameter when producing JPEG output. Must be a number
  between 1 and 100. Unfortunately, the IIIF Image API does not allow to give a JPEG quality (=compression) on the IIIF URL. Vitrinli
  allows to configure the compression quality system wide with this parameter. Allowed values are in he range
  \[1..100\] where 1 the worst quality (and highest compression factor = smallest file size) and 100 the highest
  quality (with the lowest compression factor = biggest file size). Please note that Vitrinli is not able to provide
  lossless compression for JPEG files.  
  *Cmdline option: `--quality`*  
  *Environment variable: `SIPI_JPEGQUALITY`*  
  *Default: `60`*
 
- <a name="thumbsize"></a>`thumb_size=string`: Default size for thumbnails. Parameter must be IIIF conformant size string. This configuration
  parameter can be used to define a default value for creating thumbnails. It has no direct implications but can be
  used in LUA scripts (e.g. the pre\_flight-function).  
  *Cmdline option: `--thumbsize`*  
  *Environment variable: `SIPI_THUMBSIZE`*  
  *Default: `!128,128`*

- <a name="maxtmpfileage"></a>`max_temp_file_age=num`: The maximum allowed age of temporary files (in seconds) before they are deleted.  
  *Cmdline option: `--maxtmpage`*  
  *Environment variable: `SIPI_MAXTMPAGE`*  
  *Default: `86400`* (one day)
  
#### Cache Configuration
Vitrinli uses a file-based LRU cache to store converted images, avoiding expensive re-conversions when the same
variant is requested multiple times. The cache is keyed by the canonical IIIF URL and validated against
the source file's modification time — stale entries are automatically replaced.

**Eviction:** When the cache reaches its size or file-count limit (high-water mark at 100%), LRU eviction
removes the least-recently-used entries until usage drops to 80% (low-water mark). Both limits are enforced
independently — whichever is reached first triggers eviction.

**Special values for `cache_size`:**

- `'-1'` — unlimited cache (no size eviction)
- `'0'` — cache disabled entirely (no files are cached)
- `'200M'`, `'1G'` — enforced limit with LRU eviction

**Monitoring:** Vitrinli exports its metrics over OTLP, to the endpoint in
`OTEL_EXPORTER_OTLP_ENDPOINT`; there is no `GET /metrics` scrape endpoint. The names below are what
the standard OTLP-to-Prometheus normalization produces at the collector. Cache counters
(`sipi_cache_hits_total`, `sipi_cache_misses_total`, `sipi_cache_evictions_total`, `sipi_cache_skips_total`)
and gauges (`sipi_cache_size_bytes`, `sipi_cache_files`, `sipi_cache_size_limit_bytes`, `sipi_cache_files_limit`)
report cache health. For server load and throttling, use the admission metrics
(`sipi_admission_permits_in_use` and `sipi_admission_permits_total` for saturation,
`sipi_admission_tile_waiting`/`sipi_admission_full_waiting` for per-partition queue depth,
`sipi_admission_tile_shed_total`/`sipi_admission_full_shed_total` for per-partition sheds).
Request latency is the semantic-convention histogram `http_server_request_duration_seconds`, labelled by
`http_route`, `http_request_method`, and `http_response_status_code`.
`sipi_tiff_pyramid_reduced_decodes_total` counts TIFF decodes served from a reduced pyramid level (a
zoomed-out or thumbnail request that read a smaller stored resolution instead of the full-resolution image).

The following configuration parameters determine the behaviour of the cache:

- <a name="cache_dir"></a>`cache_dir=path`: Path to the cache directory. Created automatically if missing.
  *Cmdline option: `--cachedir`*
  *Environment variable: `SIPI_CACHEDIR`*
  *Default: `./cache`*

- <a name="cache_size"></a>`cache_size=amount`: Maximum cache size. Use `'-1'` for unlimited, `'0'` to disable,
  or a size string like `'200M'` or `'1G'`. Eviction triggers at 100% and purges down to 80%.
  *Cmdline option: `--cachesize`*
  *Environment variable: `SIPI_CACHESIZE`*
  *Default: `200M`*

- <a name="cache_nfiles"></a>`cache_nfiles=num`: Maximum number of cached files. Set to `0` for no file-count limit.
  Eviction triggers when either size or file-count limit is reached.
  *Cmdline option: `--cachenfiles`*
  *Environment variable: `SIPI_CACHENFILES`*
  *Default: `200`*

!!! note "Deprecated keys"
    The old configuration keys `cachedir`, `cachesize`, and `cache_hysteresis` are still accepted
    with a deprecation warning. The `cache_hysteresis` parameter has been removed — eviction now
    always uses a fixed 80% low-water mark. See `DEPRECATIONS.md` for details.
  
#### Configuration of the HTTP File Server
Vitrinli offers  HTTP file server for HTML and other files. Files with the ending `.elua` are HTTP-files with embeded
LUA code. Everything between the <lua>...</lua> tags is interpreted as LUA code and the output embedded in the
data stream for the client.

All configurations for the HTTP server are in the `fileserver` table:

- `docroot=path`: Path to the document root of the file server.  
  *Cmdline option: `--docroot`*  
  *Environment variable: `SSIPI_DOCROOT`*  
  *Default: `./server`*
 
- `wwwroute=string`: Route for the file server should respond to requests.That is, a file with the name "dada.html"
  is accessed with `http://dnsname/server/dada.html`, if the `wwwroute`is set to `/server`.  
  *Cmdline option: `--wwwroute`*  
  *Environment variable: `SIPI_WWWROUTE`*  
  *Default: `/server`*
  
#### Configuration of Administrator Access
Vitrinli allows special administrator access for some tasks. In order to allow for this, an administrator has to be defined
as follows:
```lua
admin = {
    --
    -- username of admin user
    --
    user = 'admin',

    --
    -- Administration password
    --
    password = 'Sipi-Admin'
}
```
If You're using the administrator user, please make sure that the config file is not exposed!

#### Routing Table
Vitrinli allows to implement RESTful interfaces or other services based on LUA-scripts which are located in the scripts
directory. In order to use these LUA-scripts as endpoints, the appropriate routes have to be defined in the
`routes` table. An entry has the following form:
- `method`: the HTTP request. Supported are `GET`, `POST`, `PUT` and `DELETE`.
- `route`: A URL path that may contain `/`'s.
- `script`: Name of the LUA script in the script directory.

Thus, the routing section of a Vitrinli configuration file may look as follows:

```lua
routes = {
    {
        method = 'POST',
        route = '/api/upload',
        script = 'upload.lua'
    },
    {
        method = 'GET',
        route = '/test/orientation',
        script = 'orientation.lua'
    }
}
```
