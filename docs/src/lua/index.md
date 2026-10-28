# Vitrinli Lua Interface
Vitrinli has an embedded [LUA](http://www.lua.org) interpreter. LUA is a simple script language that was developped
specifically to be embedded into applications. For example the games [minecraft](https://www.minecraft.net) and
[World of Warcraft](https://worldofwarcraft.com/de-de/) make extensive use of LUA scripting for customization and programming extensions.

Each HTTP request to Vitrinli runs in a fresh, independent Lua instance
(language version 5.3). Therefore, LUA may be used in the following contexts:

- Preflight function
- Embedded in HTML pages
- RESTful services using the Vitrinli routing

Each lua-instance in Vitrinli includes additional Vitrinli-specific information:

- global variables about the Vitrinli configuration
- information about the current HTTP request
- Vitrinli specific functions for
  - processing the request and send back information
  - getting image information and transforming images
  - accessing SQLite databases

In general, the Vitrinli LUA function make use that a Lua function's return value may consist of
more than one element (see [Multiple Results](http://www.lua.org/pil/5.3.html)):

*Each request runs in its own thread with its own, freshly built Lua VM.
Globals do not survive across requests; within one request, the init script
and the hook or route script share the same VM and globals. The VM is
sandboxed and resource-limited — see the next section.*

## Sandbox and resource limits

Scripts run in a hardened, per-request Lua VM:

- **Standard library whitelist.** Only `string`, `table`, `math`, `utf8` and
  `package` are loaded. The `io` and `debug` libraries are absent, and the
  base-library escape hatches `dofile`, `loadfile`, `load` and `collectgarbage`
  are removed, as are `package.loadlib` and `package.searchpath`.
- **`os` shim.** The `os` table provides exactly `os.getenv`, `os.clock` and
  `os.date` (a C-locale strftime subset, including `%c`, `%x`, `%X`, the `*t`
  table form and the `!` UTC prefix). `os.execute`, `os.remove`, `os.time`
  and the rest of the stock `os` library are absent — use `server.systime()`
  for the current epoch time and the `server.fs.*` functions for file
  operations.
- **Restricted `require`.** `require 'name'` resolves plain module names
  (letters, digits and `_` only) against the configured script directory
  (`scriptdir`) and nowhere else; `package.path` and `package.cpath` are
  empty.
- **Memory cap.** Lua-heap allocations are capped per request (default
  64 MiB; operator knob `SIPI_LUA_MEMORY_LIMIT`, in bytes). An untrapped
  over-cap allocation kills the script. As in stock Lua, `pcall` can catch
  the allocation error itself — but the cap stays in force for every
  subsequent allocation.
- **Execution deadline.** Each request VM gets one wall-clock budget covering
  the init script, the hook or route script, and every binding call (default
  5 seconds; operator knob `SIPI_LUA_TIMEOUT_MS`). On expiry the script is
  killed; `pcall` cannot trap its way past the deadline, and blocking calls
  (`server.http`, sqlite) never wait past the remaining budget.
- **Kill behavior.** A script killed before any output was sent yields an
  HTTP 500 with a generic body. A script killed — or failing with an uncaught
  error — after output started has its response stream aborted (never a
  clean end-of-body). A killed `pre_flight`
  decision is never written to the preflight access-cache. Kills are counted
  in the `sipi_lua_kills_total{reason}` metric and logged.
- **Fail-closed startup.** An error in the init script refuses server
  startup. Whether `pre_flight`/`file_pre_flight` are defined is probed once
  at boot; if a later edit to the init script breaks it, affected requests
  fail with 500.
- **Bytecode cache.** The init script, route scripts and `require`d modules
  are compiled once and cached as bytecode, invalidated by file modification
  time and size — a script edit takes effect on the next request without a
  restart. Adding a *new* preflight hook still requires a restart, since the
  hook probes are boot-frozen.
- **Filesystem bindings are not path-confined.** `server.fs.*` and the other
  path-taking bindings (roughly a dozen across the API) accept any path the
  script constructs; none of them jail the result under `imgroot`,
  `scriptdir`, or any other configured root. This makes the `docroot`
  fileserver — which executes `.lua`/`.elua` scripts it finds under
  `docroot` — a remote-code-execution vector if `docroot` overlaps any root a
  request can write to. Startup refuses to start when the configured
  `fileserver.docroot` equals or nests with `imgroot`, `tmpdir`, `scriptdir`,
  or `cache_dir`; keep `docroot` disjoint from every writable or
  Lua-accessible directory. There is deliberately no `confine(path)` helper
  at the binding layer: scripts are operator-supplied, not untrusted input,
  and the startup overlap check closes the one misconfiguration that turns a
  script bug into RCE — adding per-call confinement across ~12 bindings would
  be redundant defense-in-depth.

## Preflight function
It is possible to define a LUA pre-flight function for *IIIF*-requests and independently one for *file*-requests
(indicated by a _/file_ postfix in the URL). Both are optional and are best located in the init-script (see
[configuration options](../guide/sipi.md#setup-of-vitrinli-directories) of Vitrinli). It is executed after the incoming
HTTP request data has been processed but before an action to respond to the request has been taken. It should
be noted that the pre-flight script is only executed for IIIF-specific requests (either using the IIIF URL-syntax or the
_/file_ postfix). All other HTTP requests are being directed to "normal" HTTP-server part of Vitrinli.
These can utilize the lua functionality by embedding LUA commands within the HTML.

### IIIF preflight function
The IIIF preflight function must have the name **pre_flight** with the following signature:

```lua
function pre_flight(prefix,identifier,cookie)

    return "allow", filepath
end
```
The preflight function takes 3 parameter:

- `prefix`: This is the prefix that is given on the IIIF url [mandatory]  
  *http(s)://{server}/__{prefix}__/{id}/{region}/{size}/{rotation}/{quality}.{format}*  
  Please note that the prefix may contain several "/" that can be used as path to the repository file
- `identifier`: The image identifier (which must not correspond to an actual filename in the media files repositoy
  of the Vitrinli IIIF server)
  [mandatory]
- `cookie`: A cookie containing authorization information. Usually the cookie contains a Json Web Token [optional]

The pre-flight function must return at least 2 parameters:

- `permission`: A string or a table indication the permission to read the image. In a simple case it's either the
  string `"allow"` or `"deny"`.  
  To allow more flexibility, the following permission tables are supported:  
    - Restricted access with watermark. The watermark must be a TIFF file with a single 8-bit channel (gray value
    image). For example:  
    `{ type = 'restrict', watermark = './wm/mywatermark.tif' }`
    - Restricted access with size limitation. The size must be a
      [IIIF size expression](https://iiif.io/api/image/3.0/#42-size). For example:  
     `{ type = 'restrict', size='!256,256' }`  
     The size expression turns the image into its own, smaller image of the restricted
     dimensions (RW, RH): `info.json` reports (RW, RH) as `width`/`height`, as its largest
     `sizes` entry and as `maxWidth`/`maxHeight`. Region and size in an image request
     resolve against (RW, RH), not the native pixels, and every response is cut from one
     rendering of the whole image at (RW, RH), so tiles at size `max` stitch to `full/max`
     exactly. A request whose output (before rotation) is wider than RW or taller than RH,
     `^` upscaling included, is a 400. A client cannot recover more than `full/max` by
     requesting regions.  
     The embedded size fields of a size-restricted output (EXIF `PixelXDimension`/`PixelYDimension`,
     `ImageWidth`/`ImageLength`/`ImageHeight` including maker-note ones, `RelatedImageWidth`/`RelatedImageLength`,
     `DefaultCropSize`, `ActiveArea` and the DNG `Original*Size` tags; XMP `exif:PixelXDimension`/`exif:PixelYDimension`,
     `tiff:ImageWidth`/`tiff:ImageLength` and `Iptc4xmpExt:MaxAvailWidth`/`MaxAvailHeight`) state the delivered
     size, and every embedded preview image (the EXIF thumbnail, SubIFD and maker-note previews, XMP
     `xmp:Thumbnails` and `xmpGImg:image`, the IPTC `Preview` record, and the same inside the Photoshop image resources an EXIF block can carry) is dropped; image
     resources that cannot be rewritten (unparseable, or carrying slices) fail the request with a 500. All other embedded metadata, rights and copyright included, is returned
     unchanged.  
     A watermark-only `restrict` keeps the native dimensions and coordinates.  
     A size that can never reduce an image (`max`, `pct:` of 100 or more, `red:0`) is refused when no
     watermark is set (an empty `watermark` is none): 403 on the image and on `info.json`, 404 on
     `knora.json`. A size that leaves an axis below 2 px is refused with or without a watermark. An
     image already inside a box size (e.g. 100×80 under `!128,128`) is served at its native size.
    - `"stream"` marks the representation as one to be consumed in place rather than kept as a file —
      streamed audio or video, typically. **It expresses intent and enforces nothing:** Vitrinli serves a
      `stream` decision exactly as `allow`, with the same bytes and the same headers, and only counts
      it (`sipi.preflight.decisions{permission="stream"}`). Do not rely on it as an access control.
    - Vitrinli also supports the [IIIF Authentification API](https://iiif.io/api/auth/1.0/). See section [IIIF
      Authentification]() on how to implement this feature in the pre-flight function.
- `filepath`: The path to the Service File in the media files repository. This path can be assembled using the
  `prefix` and `identifier` using any additional information (e.g. accessing a database or using the LUA restful client)
  
The most simple working pre-flight looks as follows assuming that the `identifier`is the name of the Service File
in the repository and the `prefix` is the path:
```lua
function pre_flight(prefix, identifier, cookie)
    if config.prefix_as_path then
        filepath = config.imgroot .. '/' .. prefix .. '/' .. identifier
    else
        filepath = config.imgroot .. '/' .. identifier
    end
    return 'allow', filepath
end
```
Above example preflight function allows all files to be served without restriction.

#### Preflight access-cache (opt-in) and its contract

Vitrinli can cache the `pre_flight` access decision to avoid re-running the hook for
repeated requests to the same image, which matters when the hook makes a remote
call (e.g. to an auth service). The cache is **disabled by default** and is enabled
with `--preflight-cache-ttl <secs>` / `SIPI_PREFLIGHT_CACHE_TTL` (the TTL is the
maximum staleness of a revoked or expired permission).

The cache key is `(prefix, identifier, Cookie header, Authorization header)`
**only**. A cached decision is therefore reused for any later request that matches
those fields, regardless of the rest of the request. Even though `pre_flight`
receives only `(prefix, identifier, cookie)` as arguments, the hook body can also
read the full request through the `server` bindings — `server.uri` (the complete
IIIF path, including region/size/quality), any other header via `server.header`,
the host, and the client IP. If your hook's decision depends on any of those, the
cache will serve a stale or wrong decision to a different request that shares only
the keyed fields.

> **Enable the cache only if your `pre_flight` decision is a pure function of
> `(prefix, identifier, Cookie, Authorization)`.** A hook that consults
> `server.uri`, other headers (e.g. an `X-Api-Key`), the host, or the client IP
> must leave the cache disabled (the default), or it risks returning access
> decisions made for a different request.

#### More complex example of preflight function 

The following example uses some Vitrinli lua funtions
to access an authorization server to check if the user (identified by a cookie) is allowed to see the specific image. We are
using [Json Web Tokens](https://jwt.io) (JWT) which are supported by Vitrinli specific LUA functions. Please note that the
Vitrinli JTW-functions support an arbitrary payload that has not to follow the JWT recommendations. In order to encode, the
JWT_ALG_HS256 is beeing used together with the key that is defined in the Vitrinli configuration as
[jwt_secret](../guide/sipi.md#jwt-secret).
```lua
function pre_flight(prefix, identifier, cookie) 
    --
    -- make up the file path
    --
    if config.prefix_as_path then
        filepath = config.imgroot .. '/' .. prefix .. '/' .. identifier
    else
        filepath = config.imgroot .. '/' .. identifier
    end
    --
    -- we need a cookie containing the user information that will be
    -- sent to the authorization server. The payload may be any table,
    -- but it must carry a valid 'exp' claim: server.decode_jwt requires
    -- and validates it.
    --
    if cookie then
        --
        -- we decode the cookie in order to get a table of key/value pairs
        --
        success, userinfo = server.decode_jwt(cookie)
        if not success then
            return 'deny', filepath
        end
        --
        -- prepare the RESTful call to the authorization server
        --

        -- add the image identifier to the info table:
        userinfo["imgid"] = identifier
        -- encode the userinfo to a JWT-like token:
        local new_cookie = server.generate_jwt(userinfo) 
        local url = 'http://auth.institution.org/api/getauth/' .. identifier
        local auth_information = { Cookie = new_cookie }
        --
        -- make the HTTP request with a timeout of 500 ms
        --
        success, result = server.http('GET', url, auth_information, 500) 
        if success then
            --
            -- we got a response from the server
            --
            success, response_json = server.json_to_table(result.body)
            if success then  -- everything OK
                return {
                            type = response_json.type,
                            restriction = response_json.restriction
                        }, filepath
            else
                return 'deny', filepath
            end
        else
            return 'deny', filepath
        end
    else
        return 'deny', filepath
    end
end
```
Above example assumes that the cookie data is a string that contains encrypted user data from a table (key/value pair).
Jason Web Token. This token is decoded and the information about the image to be displayed is added. Then the information
is encoded as a new token that ist transmitted to the RESTful interface of the authentification server. The answer is
assumed to be json containing information about the type ('allow', 'deny', 'restrict') and the restriction settings.
The pre-flight function uses the following Vitrinli-specific LUA global variables and function:

- [config.imgroot](#configimgroot): (Global variable) Root directory of the image repository.
- [server.http()](#serverhttp): (Function) Used to create a RESTful GET request.
- [server.generate_jwt()](#servergenerate_jwt): (Function) Create a new JWT token from a key/value table.
- [server.json_to_table()](#serverjson_to_table): (function) Convert a JSON into a LUA table.

### File preflight function
An URL in the form ```http(s)://{server}/{prefix}/{identifier}/file``` serves the given file as binary object (including
propere mimetype in the header etc.). The file has to reside in the directory tree defined for IIIF requests. In these
cases, a preflight script name `file_pre_flight` is being called if defined. Its signature is as follows:
```lua
function file_pre_flight(filepath, cookie)

end
```
A simple example allowing access only to the file _"unit/test.csv"_ would be:
```lua
function file_pre_flight(filepath, cookie)
    if filepath == "./images/unit/test.csv" then
        return "allow", filepath
    else
        return "deny", ""
    end
end
```
This script would deny all other file access and the Vitrinli IIIF server responds with a `401 Unauthorized` error.

## LUA embedded in HTML
The HTTP server that is included in Vitrinli can serve any type of file which are just transfered as is to the client.
However, if a file has an extension of `.elua`, it is assumed to be a HTML file with embedded LUA code. ALL Vitrinli-specific
LUA functions and global variables are available.

Embedding works with the special tag `<lua>` and `</lua>`. All text between the opening and closing tag is interpreted
as LUA code. Vitrinli provides an extra LUA function to output data to the client ([server.print](#serverprint)). Thus, dynamic,
server-generated HTML may be created. A sample page that displays some information about the server configuration and
client info could like follows:
```html
<html>
    <head>
        <title>SIPI Configuration Info</title>
    </head>
    <body>
    <h1>SIPI Configuration Info</h1>
    <h2>Configuration variables</h2>
    <table>
        <tr>
            <td>imgroot</td>
            <td>:</td>
            <td><lua>server.print(config.imgroot)</lua></td>
        </tr>
        <tr>
            <td>docroot</td>
            <td>:</td>
            <td><lua>server.print(server.docroot)</lua></td>
        </tr>
        <tr>
            <td>scriptdir</td>
            <td>:</td>
            <td><lua>server.print(config.scriptdir)</lua></td>
        </tr>
        <tr>
            <td>cachedir</td>
            <td>:</td>
            <td><lua>server.print(config.cache_dir)</lua></td>
        </tr>
        <tr>
            <td>tmpdir</td>
            <td>:</td>
            <td><lua>server.print(config.tmpdir)</lua></td>
        </tr>
        <tr>
            <td>port</td>
            <td>:</td>
            <td><lua>server.print(config.port)</lua></td>
        </tr>
        <lua>
            if server.has_openssl then
                server.print('<tr><td>SSL port</td><td>:</td><td>' ..
                             config.sslport .. '</td></tr>')
            end
        </lua>
        <tr>
            <td>maximal post size:</td>
            <td>:</td>
            <td><lua>server.print(config.max_post_size)</lua></td>
        </tr>
    </table>
    <h2>Client information</h2>
    <table>
        <tr>
            <td>Host in request</td>
            <td>:</td>
            <td><lua>server.print(server.host)</lua></td>
        </tr>
        <tr>
            <td>IP of client</td>
            <td>:</td>
            <td><lua>server.print(server.client_ip)</lua></td>
        </tr>
        <tr>
            <td>URL path</td>
            <td>:</td>
            <td><lua>server.print(server.uri)</lua></td>
            </tr>
    </table>

    <p>Important Note: "IP of client" and "Host in request" may
       indicate the information of a proxy and notof the actual
       client!</p>
    <h2>Request Header Information</h2>
    <table>
        <lua>
            for key, val in pairs(server.header) do
                server.print('<tr><td>' .. key ..
                             '</td><td>:</td><td>' .. val .
                             '</td></tr>')
            end
        </lua>
    </table>
    </body>
</html>
```

### Embedded LUA and enforcing SSL
The supplied example initialization file offers a LUA function that enforces the use of a SSL encryption page
proteced by a user name and password. It is used as follows by adding the following code
*before the `<html>` opening tag*:

```lua
<lua>
    if server.secure then
        protocol = 'https://'
    else
        protocol = 'http://'
    end

    success,token = authorize_page('admin.sipi.org',
                                   'administrator',
                                    extecteduser, expectedPassword)
    if not success then
        return
    end
</lua>
```
where `expectedUser` and `extectedPassword` are the user/password combination the user is expected to enter.

### File uploads to Vitrinli

The Vitrinli specific LUA function allow the upload of files using POST requests with `multipart/form-data` content.
The global variable `server.uploads` contains`the information about the uploads. The following variables and
function help to deal with uploads:

- [server.uploads](#serveruploads) : information about the files in the upload request.
- [server.copyTmpfile](#servercopytmpfile) : copies a fie from the upload location to the destination directory.

In addition the file system functions that Vitrinli provides may be used.

See the scripts `upload.elua` and `do-upload.elua` in the server directory, and `upload.lua` in the scripts directory
for a working example.

## RESTful API and custom routes

Custom routes to implement a RESTful API can be defined in Vitrinli's configuration file using the
`routes` configuration variable. For example:

    routes = {
        {
            method = 'GET',
            route = '/status',
            script = 'get_repository_status.lua'
        },
        {
            method = 'POST',
            route = '/make_thumbnail',
            script = 'make_image_thumbnail.lua'
        }
    }

Vitrinli looks for these scripts in the directory specified by `scriptdir`
in its configuration file. The first route that matches the beginning of
the requested URL path will be used.

> **Do not pass secrets in query strings.** When tracing is enabled, the
> request URL — **including its query string** — is recorded on the exported
> span (the OpenTelemetry HTTP semantic conventions capture `url.query`). A
> custom route or preflight script that reads a token, password, or other
> secret from a query-string parameter will leak it into your traces. Carry
> secrets in a `Cookie` or `Authorization` header (the `pre_flight` examples
> above use a cookie), never in the URL.

## IIIF Authentication API 1.0 in Vitrinli
The `pre_flight` function is also responsible for activating the IIIF Auth API. In order to do so, the pre_flight script
returns a table that contains all necessary information. For details about the IIIF Authentication API 1.0 see the
[IIIF documentation](https://iiif.io/api/auth/1.0/). The following fields have to be returned by the
`pre_flight`-function as LUA-table:

- `type`: String giving the type. Valid are:  
  `"login"`, `"clickthrough"`, `""kiosk"` or `"external"`.
- `cookieUrl`: URL where to get a valid IIIF Auth cookie for this service.
- `tokenUrl`: URL where to get a valid IIIF Auth token for this service.
- `confirmLabel`: Label to display in confirmation box.
- `description`: Description for login window.
- `failureDescription`: Information, if login fails.
- `failureHeader`: Header for failure window.
- `header`: Header of login window
- `label`: Label of the login window

In addition, the filepath has to be returns. A full response may look as follows:

```lua
return {
   type = 'login',
    cookieUrl = 'https://localhost/iiif-cookie.html',
    tokenUrl = 'https://localhost/iiif-token.php',
    confirmLabel =  'Login to SIPI',
    description = 'This Example requires a demo login!',
    failureDescription = '<a href="http://example.org/policy">Access Policy</a>',
    failureHeader = 'Authentication Failed',
    header = 'Please Log In',
    label = 'Login to SIPI',
}, filepath
```

Vitrinli will use this information returned by the `pre_flight` function to return the appropriate responses to the
client requests based on the IIIF Authentication API 1.0. Check for support of the IIIF Authentication API 1.0
for [mirador](https://projectmirador.org) and [universalviewer](https://universalviewer.io), both applications which
suppport the IIIF standards.


## Vitrinli variables available to Lua scripts
There are many globally accessible LUA variables made available which reflext the configuration of Vitrinli and the
state of the server and request. This variables a read only and created for every request.

### Vitrinli configuration variables
This variables are defined ither in the configuration file if Vitrinli, in environemt variables at startup or
as command line option when starting the server.

#### config.hostname

    config.hostname

The configured hostname, exposed to Lua route scripts (e.g. to build absolute
preview URLs). Defaults to `localhost`; not used by the server itself.

#### config.port

    config.port
    
Portnumber where the Vitrinli server listens
(see [serverport](../guide/sipi.md#portnum) in configuration description).

#### config.sslport

    config.sslport
    
Portnumber for SSL connections of Vitrinli
(see [sslport](../guide/sipi.md#sslport) in configuration description).

####config.imgroot

    config.imgroot
    
Root directory for IIIF-served images
(see [imgroot](../guide/sipi.md#imgroot) in configuration description).

#### config.docroot

    config.docroot

Root directory for WEB-Server
(see [docroot](../guide/sipi.md#docroot) in configuration description).

#### config.max\_temp\_file\_age

    config.max_temp_file_age
    
maximum age of temporary files
(see [max_temp_file_age](../guide/sipi.md#maxtmpfileage) in configuration description).

#### config.prefix\_as\_path

    config.prefix_as_path`
    
`true` if the prefix should be used as path info
(see [prefix_as_path](../guide/sipi.md#prefixaspath) in configuration description).

#### config.init\_script

    config.init_script
    
Path to initialization script
(see [initscript](../guide/sipi.md#scriptinit) in configuration description).

#### config.scriptdir

    config.scriptdir

Path to script directory.
(see [scriptdir](../guide/sipi.md#scriptdir) in configuration description).

#### config.cache\_dir

    config.cache_dir
    
Path to cache directory for iIIF served images.
(see [cachedir](../guide/sipi.md#cachedir) in configuration description).

#### config.cache\_size

    config.cache_size
    
Maximal size of cache
(see [cachesize](../guide/sipi.md#cachesize) in configuration description).

#### config.cache\_n\_files

    config.cache_n_files
    
Maximal number of files in cache.
(see [cache_nfiles](../guide/sipi.md#cachenfiles) in configuration description).

#### config.jpeg\_quality

    config.jpeg_quality
    
Unfortunately, the IIIF Image API does not allow to give a JPEG quality (=compression) on the IIIF URL. Vitrinli
allows to configure the compression quality system wide with this parameter. Allowed values are in he range
\[1..100\] where 1 the worst quality (and highest compression factor = smallest file size) and 100 the highest
quality (with lowest compression factor = biggest file size). Please note that Vitrinli is not able to provide
lossless compression for JPEG files.
(see [jpeg_quality](../guide/sipi.md#jpegquality) in configuration description).

#### config.thumb\_size

    config.thumb_size
    
Default thumbnail image size.
(see [thumb_size](../guide/sipi.md#thumbsize) in configuration description).

#### config.max\_post\_size

    config.max_post_size
    
Maximal size of POST data allowed
(see [max_post_size](../guide/sipi.md#maxpostsize) in configuration description).

#### config.tmpdir

    config.tmpdir
    
Temporary directory to store uploads.
(see [tmpdir](../guide/sipi.md#tmpdir) in configuration description).

### Vitrinli Server Variables
Vitrinli server variables are dependent on the incoming request and are created by Vitrinli automatically for each
request.

#### server.method

    server.method
    
The HTTP request method. Is one of `OPTIONS`, `GET`, `HEAD`, `POST`, `PUT`, `DELETE`, `TRACE`, `CONNECT` or `OTHER`.

#### server.has\_openssl

    server.has_openssl
    
`true` if OpenSSL is available. This variable is determined compilation time. Usually SSL should be included, but Vitrinli
can be compiled without SSL support. There is no option in the configuration file for this.

#### server.secure

    server.secure
    
`true` if the connection was made over HTTPS using SSL.

#### server.host

    server.host
    
The hostname of the Vitrinli server that was used in the request.

#### server.client\_ip

    server.client_ip
    
The IPv4 or IPv6 address of the client connecting to Vitrinli.

#### server.client\_port

    server.client_port
    
The port number of the client socket.

#### server.uri

    server.uri
    
The URL path used to access Vitrinli (does not include the hostname).

#### server.header

    server.header
    
A table containing all the HTTP request headers(in lowercase).

#### server.cookies

    server.cookies
     
A table of the cookies that were sent with the request — one entry per
cookie, keyed by the cookie's name in its original case.

#### server.get

    server.get

A table of GET request parameters.

#### server.post

    server.post
    
A table of POST or PUT request parameters.

#### server.request

    server.request

All request parameters.

#### server.content

    server.content
    
If the request had a body, the variable contains the body data. Otherwise it's `nil`.

#### server.content\_type

    server.content_type

Returns the content type of the request. If there is no type or no content, this variable is `nil`.

#### server.docroot

    server.docroot

The document root of the built-in web server, when one is configured;
`nil` otherwise.

#### server.uploads

    server.uploads
    
This is an array of upload parameters, one per file. Each one is a table containing:

- `fieldname`: the name of the form field.
- `origname`: the original filename.
- `tmpname`: a temporary path to the uploaded file.
- `mimetype`: the MIME type of the uploaded file as provided by the browser.
- `filesize`: the size of uploaded file in bytes.

The upload can be accessed as follows:
```lua
for index, value in pairs(server.uploads) do
    --
    -- copy the uploaded file to the image repository using the original name
    --
    server.copyTmpfile(index, config.imgdir .. '/' .. value["origname"])
end
```

### Knora-specific variables
The development of Vitrinli came out of the need to have a flexible, high performance IIIF server for the Swiss National
research infrastructure [Data and Service Center for the Humanities](https://dasch.swiss) (DaSCH). The aim of the DaSCH
is to guarantee long-term accessibility of research data from the Humanities. The operates a specialized platform
[Knora](https://knora.org). The following variables are for internal use only.

#### config.knora\_path

    config.knora_path
    
Path to knora REST API (only for Vitrinli used with Knora)

#### config.knora\_port

    config.knora_port
    
Port that the Knora API uses


## Vitrinli functions available to Lua scripts

Vitrinli provides the following functions that can be called from Lua
scripts. Each function returns two values. The first value is `true` if
the operation succeeded, `false` otherwise. If the operation succeeded,
the second value is the result of the operation, otherwise it is an
error message.

### Vitrinli Connection Functions
These LUA function alter the way the HTTP connection is handled.

#### server.setBuffer

    success, errmsg = server.setBuffer([bufsize][,incsize])

Accepted for compatibility: the response is streamed by the server, so the
call validates its arguments and succeeds without changing behavior.
Optionally the buffer size and increment size can be given. Returns
`true, nil` on success or `false, errormsg` on invalid arguments.

#### server.sendHeader

    success, errormsg = server.sendHeader(key, value)

Sets an HTTP response header. Returns `true, nil` on success or
`false, errormsg` on failure.

#### server.sendCookie

    success, errormsg = server.sendCookie(key, value [, options-table])

Sets a cookie in the HTTP response. Returns `true, nil` on success or
`false, errormsg` on failure. The optional `options-table` is a Lua
table containing the following keys:

-   `path`
-   `domain`
-   `expires` (value in seconds)
-   `secure` (boolean)
-   `http_only` (boolean)
-   `same_site` (string)

Response cookies default to `Secure; HttpOnly; SameSite=Lax`. The `http_only`
and `same_site` options can override those defaults (e.g. `same_site = ""`
clears `SameSite`); `secure` only ever turns the flag on.

#### server.sendStatus

    server.sendStatus(code)

Sends an HTTP status code. This function is always successful and
returns nothing.

#### server.print

    success, errormsg = server.print(values)

Prints variables and/or strings over the HTTP connection to the client that originated the request. Returns
`true, nil` on success or `false, errormsg` on failure.

#### server.requireAuth

    success, table = server.requireAuth()

This function retrieves HTTP authentication data that was supplied after sending a `'WWW-Authenticate'`-header (e.g.
by issuing a the following commands to enter the HTTP login dialog:

        server.setBuffer()
        server.sendStatus(401);
        server.sendHeader('WWW-Authenticate', 'Basic realm="Sipi"')

It returns `true, table` on success or `false, errormsg` on failure. The result of the authorization
is returned as table with the following elements:

- `status`: Either `BASIC`, `BEARER`, `NOAUTH` (no authorization header) or `ERROR`
- `username`: A string containing the supplied username (only existing if stats is `BASIC`)
- `password`: A string containing the supplied password (only existing if stats is `BASIC`)
- `token`: A string containing the raw token information (only if status `BEARER`)
- `message`: A string containing the error message (only if status `ERROR`)

Example:

    success, auth = server.requireAuth()
    if not success then
        server.sendStatus(501)
        server.print("Error in getting authentication scheme!")
        return -1
    end

    if auth.status == 'BASIC' then
        --
        -- everything OK, let's create the token for further
        -- calls and ad it to a cookie
        --
        if auth.username == os.getenv('SIPI_ADMIN_USER') and
           auth.password == os.getenv('SIPI_ADMIN_PASSWORD') then
            tokendata = {
                iss = "sipi.unibas.ch",
                aud = "knora.org",
                user = auth.username,
                exp = server.systime() + 3600
            }
            success, token = server.generate_jwt(tokendata)
            if not success then
                server.sendStatus(501)
                server.print("Could not generate JWT!")
                return -1
            end
            success, errormsg = server.sendCookie('sipi',
                                                  token,
                                                  {path = '/', expires = 3600})
            if not success then
                server.sendStatus(501)
                server.print("Couldn't send cookie with JWT!")
                return -1
            end
        else
            server.sendStatus(401)
            server.sendHeader('WWW-Authenticate', 'Basic realm="Sipi"')
            server.print("Wrong credentials!")
            return -1
        end
    elseif auth.status == 'BEARER' then
        success, jwt = server.decode_jwt(auth.token)
        if not success then
            server.sendStatus(501)
            server.print("Couldn't deocde JWT!")
            return -1
        end
        if (jwt.iss ~= 'sipi.unibas.ch') or
           (jwt.aud ~= 'knora.org') or
           (jwt.user ~= os.getenv('SIPI_ADMIN_USER')) then
            server.sendStatus(401)
            server.sendHeader('WWW-Authenticate', 'Basic realm="Sipi"')
            return -1
        end
    elseif auth.status == 'NOAUTH' then
        server.setBuffer()
        server.sendStatus(401);
        server.sendHeader('WWW-Authenticate', 'Basic realm="Sipi"')
        return -1
    else
        server.sendStatus(401)
        server.sendHeader('WWW-Authenticate', 'Basic realm="Sipi"')
        return -1
    end


### Vitrinli File System Function
These functions offer tools to manipuale files and directories, and to gather file information.

#### server.fs.ftype

    success, filetype = server.fs.ftype(filepath)

Checks the filetype of a given filepath. Returns either `true, filetype`
(with filetype one of `"FILE"`, `"DIRECTORY"`, `"CHARDEV"`, `"BLOCKDEV"`, `"LINK"`,
`"FIFO"`, `"SOCKET"` or `"UNKNOWN"`) or `false, errormsg`.

#### server.fs.modtime

    success, modtime = server.fs.modtime(filepath)

Retrieves the last modification date of a file in seconds since epoch
UTC. Returns either `true`, `modtime` or `false`, `errormsg`.

#### server.fs.is\_readable

    success, readable = server.fs.is_readable(filepath)

Checks if a file is readable. Returns `true, readable` (boolean) on
success or `false, errormsg` on failure.

#### server.fs.is\_writeable

    success, writeable = server.fs.is_writeable(filepath)

Checks if a file is writeable. Returns `true, writeable` (boolean) on
success or `false, errormsg` on failure.

#### server.fs.is\_executable

    success, errormsg = server.fs.is_executable(filepath)

Checks if a file is executable. Returns `true, executable` (boolean) on
success or `false, errormsg` on failure.

#### server.fs.exists

    success, exists = server.fs.exists(filepath)

Checks if a file exists. Checks if a file exists. Returns `true, exists`
(boolean) on success or `false, errormsg` on failure.

#### server.fs.unlink

    success, errormsg = server.fs.unlink(filename)

Deletes a file from the file system. The file must exist and the user
must have write access. Returns `true, nil` on success or
`false, errormsg` on failure.

#### server.fs.mkdir

    success, errormsg = server.fs.mkdir(dirname, mode)

Creates a new directory with the specified permissions. Both parameters are
required; `mode` is a Unix permission integer (e.g. `tonumber('0755', 8)`).
Returns `true, nil` on success or `false, errormsg` on failure.

#### server.fs.rmdir

    success, errormsg = server.fs.rmdir(dirname)

Deletes a directory. Returns `true, nil` on success or `false, errormsg`
on failure.

#### server.fs.getcwd

    success, curdir = server.fs.getcwd()

Gets the current working directory. Returns `true, current_dir` on
success or `false, errormsg` on failure.

#### server.fs.readdir

    success, filenames = server.fs.readdir(dirname)

Gets the names of the files in a directory, not including `.` and `..`.
Returns `true, table` on success or `false, errormsg` on failure.

#### server.fs.copyFile

    success, errormsg = server.fs.copyFile(source, destination)

Copies a file from source to destination. Returns `true, nil`on success
or `false, errormsg` on failure.

#### server.fs.moveFile

    success, errormsg = server.fs.moveFile(from, to)

Moves a file. The move connot cross filesystem boundaries! `true, nil`on
success or `false, errormsg` on failure.

### Other Helper Function

#### server.http

    success, result = server.http(method, "http://server.domain[:port]/path/file" [, header] [, timeout])

Performs an outbound HTTP request. Currently implements only GET requests.
Parameters:

-   `method`: The HTTP request method. Currently must be `"GET"`.
-   `url`: The HTTP URL.
-   `header`: An optional table of key-value pairs representing HTTP
    request headers.
-   `timeout`: An optional number of milliseconds for the *whole* request
    (connect, send and receive; default 2000). The effective timeout is
    additionally capped by the remaining execution deadline of the request,
    so an outbound call can never outlive its request budget.

Hardened semantics:

-   Redirects are **not** followed: a `3xx` response is returned to the
    script as-is (with its `location` header).
-   The response body is capped at 16 MiB; a larger body fails the call.
-   Authentication is not supported.

When Vitrinli is handling a request under an active distributed trace, `server.http`
automatically adds a W3C `traceparent` header to the outbound request so the
downstream service continues the same trace (e.g. a `pre_flight` authorization
call to dsp-api). If the `header` table already contains a `traceparent`, that
value is kept and no header is injected.

On success the result is a table:

    result = {
        status_code = value, -- HTTP status code returned
        header = {
            name = value [, name = value, ...]
        },
        body = data,
        duration = milliseconds
    }

On failure the second return value is the error message string.

Example:

    success, result = server.http("GET", "http://www.salsah.org/api/resources/1", 100)

    if success then
       server.print("Status: ", result.status_code, "<br/>")
       server.print("Duration: ", result.duration, " ms<br/><hr/>")
       server.print("Body:<br/>", result.body)
    else
       server.print("ERROR: ", result)
    end

#### server.table\_to\_json

    success, jsonstr = server.table\_to\_json(table)

Converts a (nested) Lua table to a JSON string. Returns `true, jsonstr`
on success or `false, errormsg` on failure.

#### server.json\_to\_table

    success, table = server.json_to_table(jsonstr)

Converts a JSON string to a (nested) Lua table. Returns `true, table` on
success or `false, errormsg` on failure.

#### server.generate\_jwt

    success, token = server.generate_jwt(table)

Generates a [JSON Web Token](https://jwt.io/) (JWT) with the supplied table
as payload, signed with HS256 using the configured
[jwt_secret](../guide/sipi.md#jwt-secret). Returns `true, token` on success
or `false, errormsg` on failure. The payload may contain arbitrary keys
and/or the JWT claims as follows. (The type `IntDate` is a number of seconds
since 1970-01-01T0:0:0Z):

-   `iss` (string =&gt; StringOrURI) OPT: principal that issued the JWT.
-   `exp` (number =&gt; IntDate): expiration time on or after which
    the token MUST NOT be accepted for processing. **Required** in any token
    that will be verified with `server.decode_jwt`.
-   `nbf` (number =&gt; IntDate) OPT: identifies the time before which
    the token MUST NOT be accepted for processing.
-   `iat` (number =&gt; IntDate) OPT: identifies the time at which the
    JWT was issued.
-   `aud` (string =&gt; StringOrURI) OPT: identifies the audience that
    the JWT is intended for. The audience value is a string, typically
    the base address of the resource being accessed, such as
    `https://contoso.com`.
-   `prn` (string =&gt; StringOrURI) OPT: identifies the subject of
    the JWT.
-   `jti` (string =&gt; String) OPT: provides a unique identifier for
    the JWT.

#### server.decode\_jwt

    success, table = server.decode_jwt(token)

Decodes and verifies a [JSON Web Token](https://jwt.io/) (JWT) and returns
its content as table. Returns `true, table` on success or `false, errormsg`
on failure. Verification is strict:

-   The signature must be a valid HS256 signature over the configured
    [jwt_secret](../guide/sipi.md#jwt-secret); tokens using any other
    algorithm (including `none`) are rejected.
-   The token must carry an `exp` claim that is not in the past (a small
    clock-skew leeway is applied).

#### server.parse\_mimetype

    success, mimetype = server.parse_mimetype(str)
    
Parses a mimtype HTTP header string and returns a pair containing the actual
mimetype and the charset used (if available). It returns `true, pair` with pair as mimetype
and charset on success, `false, errormsg` on failure.

#### server.file\_mimetype

    success, table = server.file_mimetype(path)
    success, table = server.file_mimetype(index)

Determines the mimetype of a file. The *first* form is used if the file
path is known. The *second* form can be used for uploads by passing the
upload file index. It returns `true, table` on success or `false, errormsg` on
failure. The table has 2 members: - `mimetype` - `charset`

#### server.file\_mimeconsistency

    success, is_consistent = server.file_mimeconsistency(path)
    success, is_consistent = server.file_mimeconsistency(index)
    
Checks if the file extension and the mimetype determined by the magic
of the file is consistent. The *first* form requires a path (including the
filename with extension), the *second* can be used for checking uploads by 
passing the file index. It returns `true, is_consistent` on success or
`false, errormsg` in case of an error. `is_consistent` is true if the
mimetype corresponds to the file extension.

#### server.copyTmpfile

    success, errormsg = server.copyTmpfile(from, to)

Vitrinli saves each uploaded file in a temporary location (given by the
config variable `tmpdir`) and deletes it after the request has been
served. This function is used to copy the file to another location where
it can be retrieved later. Returns `true, nil` on success or
`false, errormsg` on failure.

Parameters:

- `from`:    an index (integer value) of array server.uploads.
- `target`:  destination path

#### server.systime

    systime = server.systime()

Returns the current system time on the server in seconds since epoch.

#### server.log

    server.log(message, loglevel)

Writes a message to the built-in logger. Severity levels are:

-   `server.loglevel.LOG_EMERG`
-   `server.loglevel.LOG_ALERT`
-   `server.loglevel.LOG_CRIT`
-   `server.loglevel.LOG_ERR`
-   `server.loglevel.LOG_WARNING`
-   `server.loglevel.LOG_NOTICE`
-   `server.loglevel.LOG_INFO`
-   `server.loglevel.LOG_DEBUG`

#### server.secure\_equals

    equal = server.secure_equals(a, b)

Constant-time string comparison for credential checks (e.g. Basic-auth
username/password), to avoid leaking timing information through an
early-exit `==` comparison. Returns a boolean.

#### server.uuid

    success, uuid = server.uuid()

Generates a random UUID version 4 identifier in canonical form, as
described in [RFC 4122](https://tools.ietf.org/html/rfc4122). Returns
`true, uuid` on success or `false, errormsg` on failure.

#### server.uuid62

    success, uuid62 = server.uuid62()

Generates a Base62-encoded UUID. Returns `true, uuid62` on success or
`false, errormsg` on failure.

#### server.uuid\_to\_base62

    success, uuid62 = server.uuid_to_base62(uuid)

Converts a canonical UUID string to a Base62-encoded UUID. Returns
`true, uuid62` on success or `false, errormsg` on failure.

#### server.base62\_to\_uuid

    success, uuid = server.base62_to_uuid(uuid62)

Converts a Base62-encoded UUID to canonical form. Returns `true, uuid`
on success or `false, errormsg` on failure.


## Helper Functions

The `helper` table carries Vitrinli-internal helpers.

#### helper.filename\_hash

```lua
success, hashed_path = helper.filename_hash(filename)
```

Converts a filename into Vitrinli's hashed storage path (the on-disk layout the
repository uses). Returns `true, hashed_path` on success or
`false, errormsg` on failure.

