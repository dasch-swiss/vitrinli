# Overview

Vitrinli is a multithreaded, high-performance, IIIF compatible media server developed by
the [Data and Service Center for the Humanities](https://dasch.swiss) at the
[University of Basel](https://www.unibas.ch/en.html). It is designed to
be used by archives, libraries, and other institutions that need to
preserve high-quality images while making them available online.

Vitrinli implements the Image API 3.0 of the International Image Interoperability Framework
([IIIF](http://iiif.io/)), and efficiently converts between image
formats, preserving metadata contained in image files. In particular, if
images are stored in [JPEG 2000](https://jpeg.org/jpeg2000/) format,
Vitrinli can convert them on the fly to formats that are commonly used on
the Internet. Vitrinli offers a flexible framework for specifying
authentication and authorization logic in [Lua](https://www.lua.org/)
scripts, and supports restricted access to images, either by reducing
image dimensions or by adding watermarks. It can easily be integrated
with [Knora](http://www.knora.org/). In addition Vitrinli preserves most of
the [EXIF](http://www.exif.org),
[IPTC](https://iptc.org/standards/photo-metadata/iptc-standard/) and
[XMP](http://www.adobe.com/products/xmp.html) metadata and can preserve
or transform [ICC](https://en.wikipedia.org/wiki/ICC_profile) color
profiles.

In addition, a simple webserver is integrated. The server is able to
serve most common file types. In addition Lua scripts and embedded Lua
(i.e., Lua embedded into HTML pages using the tags
&lt;lua&gt;…&lt;/lua&gt; are supported.

Vitrinli can also be used from the command line to convert images to/from
TIFF, [JPEG 2000](https://jpeg.org/jpeg2000/), JPEG, PNG, and WebP
formats. For all these conversions, Vitrinli tries to preserve all embedded
metadata such as
- [IPTC](https://iptc.org/standards/photo-metadata/iptc-standard/)
- [EXIF](http://www.exif.org)
- [XMP](http://www.adobe.com/products/xmp.html)
- [ICC](https://en.wikipedia.org/wiki/ICC_profile) color profiles.
However, due to the limitations of some file formats, it cannot be
guaranteed that all metadata and ICC profiles are preserved.
- [JPEG2000](https://jpeg.org/jpeg2000/) (J2k) does not allow all types of ICC profiles
  profiles. Unsupported profile types will be added to the J2k header as comment and will be
reinstated if the J2k file is converted back to the TIFF-format.

Vitrinli is a [free software](http://www.gnu.org/philosophy/free-sw.en.html),
released under the [GNU Affero General Public
License](http://www.gnu.org/licenses/agpl-3.0.en.html). It is written in
C++ and runs on Linux and macOS. Note: In order to compile Vitrinli, the user has
to provide a licensed source of the [kakadu software](https://kakadusoftware.com).

Vitrinli runs on Linux (including Debian, Ubuntu, and CentOS) and macOS.

Freely distributable binary releases are available as Docker images:
[daschswiss/sipi](https://hub.docker.com/r/daschswiss/sipi).
