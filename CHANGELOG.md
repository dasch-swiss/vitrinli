# Changelog

## [10.1.0](https://github.com/dasch-swiss/vitrinli/compare/v10.0.1...v10.1.0) (2026-11-12)


### Features

* **bazel:** Run local macOS builds on the NativeLink RBE backend ([2f8e221](https://github.com/dasch-swiss/vitrinli/commit/2f8e221e1559816539ab43b7f87cd22e210ae951))


### Bug Fixes

* **deps:** Raise rustix to 1.1.6 past RUSTSEC-2026-0486 ([1ff668e](https://github.com/dasch-swiss/vitrinli/commit/1ff668eedc0eeef453b331b98760bd1a914d2dcd))
* **docker:** Ship patched Debian packages in the production image ([d4b8cb9](https://github.com/dasch-swiss/vitrinli/commit/d4b8cb9f85ac9111b79df23086753384546b07c6))


### Code Refactoring

* **cache:** Name the cache index key after the glossary's Cache key ([1ae30ba](https://github.com/dasch-swiss/vitrinli/commit/1ae30bacb4b7b6c4502654a359ba6efb0fb1eb81))


### Documentation

* **docs:** Align the spec layout, citation rule and PR template with the eng plugin ([e38b028](https://github.com/dasch-swiss/vitrinli/commit/e38b028c00dfd9a6026e26ea027da911ef1216cf))
* **docs:** Close the agent-readiness documentation gaps ([4f12f6e](https://github.com/dasch-swiss/vitrinli/commit/4f12f6e98d8b34b88267cc7aca7210d9101f048e))
* **docs:** Fold UBIQUITOUS_LANGUAGE.md into CONTEXT.md ([64025dd](https://github.com/dasch-swiss/vitrinli/commit/64025dd4af8d74cf786e2480e4fc8eeb20a7b3d0))
* **docs:** State that local RBE use is opt-in through rbe-darwin ([352484b](https://github.com/dasch-swiss/vitrinli/commit/352484b9159361394161fed61d657e0fe3c55e78))
* **server,cli,ffi:** Describe the Rust shell as the production server ([3ee6bbf](https://github.com/dasch-swiss/vitrinli/commit/3ee6bbf4b1207ba4cc8760a4a4a4cd33e98cc3ae))
* **specs:** Add the DEV-7645 agent-readiness journal ([022c949](https://github.com/dasch-swiss/vitrinli/commit/022c94961d68554274008941ab95daf2af61837e))
* **specs:** Plan and journal local macOS builds on the NativeLink RBE backend ([74255a2](https://github.com/dasch-swiss/vitrinli/commit/74255a2396ecffec7029e2e505293fbdb95d137b))


### Miscellaneous Chores

* **ci:** Add the readiness regression check ([484e94e](https://github.com/dasch-swiss/vitrinli/commit/484e94e98a9383e40b9c45e7dd49e0a6ea1f9d6b))
* **ci:** Gate the image on fixed High/Critical CVEs in its OS packages ([8d71101](https://github.com/dasch-swiss/vitrinli/commit/8d71101d755743ba8f7dad933c2db8b66ef59596))
* **ci:** Run the darwin-arm64 test leg on macos-15 ([2a1c681](https://github.com/dasch-swiss/vitrinli/commit/2a1c6814d3e91066192d73257f2185e7c672f810))
* **deps:** Ignore the actix advisories from the uncompiled sentry-actix ([2bbde42](https://github.com/dasch-swiss/vitrinli/commit/2bbde42f858c499a7334147a3ce1a8f2dec743db))

## [10.0.1](https://github.com/dasch-swiss/vitrinli/compare/v10.0.0...v10.0.1) (2026-11-03)


### Bug Fixes

* **deps:** Raise tokio past RUSTSEC-2026-0386 ([64c7690](https://github.com/dasch-swiss/vitrinli/commit/64c769088eb219444c84358bf702a469f98de619))
* **format_handlers:** Reject a JPEG 2000 region that reduces to no samples ([e7aba1c](https://github.com/dasch-swiss/vitrinli/commit/e7aba1cca3627054c0e6569cabecb2633795273f))
* **server:** Send CORS headers on every IIIF-route response ([4fd154f](https://github.com/dasch-swiss/vitrinli/commit/4fd154f4e81489c748652cd0facd7b7733c6ded8))


### Documentation

* **docs:** Describe how maintainers fix a vulnerability privately ([5616e30](https://github.com/dasch-swiss/vitrinli/commit/5616e3058abacf17ad8890680e580f7ea0c22732))
* **docs:** Rename the product from SIPI to Vitrinli ([2cf2b55](https://github.com/dasch-swiss/vitrinli/commit/2cf2b5557ae1d99578516ccbda99323113b40698))
* **docs:** Stack dependent PRs with gh stack ([cf8ec44](https://github.com/dasch-swiss/vitrinli/commit/cf8ec44dc1242d30122f691dcbd37e31e9ae7fca))
* **specs:** Add the Vitrinli rename plan ([fd56024](https://github.com/dasch-swiss/vitrinli/commit/fd560247a85dd7ef537d890413f693f4e7a125bb))


### Build System

* **docker:** Label the image Vitrinli from dasch-swiss/vitrinli ([660128f](https://github.com/dasch-swiss/vitrinli/commit/660128f00871da94f24569dcfec8eb41d85c171d))


### Miscellaneous Chores

* **ci:** Point the Codecov upload at dasch-swiss/vitrinli ([4efe043](https://github.com/dasch-swiss/vitrinli/commit/4efe043350e6bedf8a53e048586b8d6126944942))
* **ci:** Turn off Dependabot ([1ce86a2](https://github.com/dasch-swiss/vitrinli/commit/1ce86a2c4c6b799a2829de36c1a36587607e8a68))
* **deps:** Ignore the crypto-bigint and block-buffer advisories ([80aa0e0](https://github.com/dasch-swiss/vitrinli/commit/80aa0e0e0d7af8660744afcfbf3f32475074cf07))

## [10.0.0](https://github.com/dasch-swiss/sipi/compare/v9.1.1...v10.0.0) (2026-10-22)


### ⚠ BREAKING CHANGES

* **ffi:** region and size on a size-restricted image resolve in the restricted image's coordinate space (RW, RH), not in native pixels; any restricted output larger than (RW, RH) before rotation is a 400. A DSP deployment must move its viewer to info.json tiling in the same release that takes this version.

### Bug Fixes

* **deps:** Raise rustls, quinn-proto and webpki crates past open advisories ([f5596a3](https://github.com/dasch-swiss/sipi/commit/f5596a32b7de0462a76fe9151d0bfcf74b332589))
* **ffi,server:** Decide restricted-view dimensions once in the shell ([ca84744](https://github.com/dasch-swiss/sipi/commit/ca8474436cb1e2bacbc1f770a4614c0c59bb09b8))
* **ffi:** Rewrite embedded dimension metadata in size-restricted outputs ([ef4f350](https://github.com/dasch-swiss/sipi/commit/ef4f350a0b25f3ee1ff4d68c80b5572ae89de3c8))
* **ffi:** Serve a size-restricted image as its own image ([d88f6e9](https://github.com/dasch-swiss/sipi/commit/d88f6e90f8aa8e3d38e450bbbe2930a5a15885c9))
* **format_handlers,image_processing:** Convert YCbCr JPEG 2000 images in their declared channel order ([1661636](https://github.com/dasch-swiss/sipi/commit/16616360427d656502a9aa74fa4e362fa1675dfa))
* **format_handlers:** Decode subsampled JPEG 2000 components instead of spinning ([f92245c](https://github.com/dasch-swiss/sipi/commit/f92245c532d6c9c1d914aadfa8f5928838d9ec5b))
* **format_handlers:** Keep IPTC and oversized metadata blocks in JPEG output ([9481366](https://github.com/dasch-swiss/sipi/commit/9481366c91f9de8997b8a5f864b32d6f7dbee149))
* **format_handlers:** Truncate an existing file when writing a JPEG ([9999696](https://github.com/dasch-swiss/sipi/commit/9999696eda25c888b98ae118e1cedd64d9482089))
* **format_handlers:** Zero the holes a streamed TIFF seeks over ([7d7839f](https://github.com/dasch-swiss/sipi/commit/7d7839f6209a41775b8366afdb4818bf0819ca72))
* **metadata:** Replace an existing EXIF entry in addKeyVal instead of appending ([209eed3](https://github.com/dasch-swiss/sipi/commit/209eed30ac73aeecda425fbe0b23be9c54157979))
* **server:** Echo the Origin with credentials on info.json ([824f0cf](https://github.com/dasch-swiss/sipi/commit/824f0cf6c17fbb06d32b04dd840229fe3515399a))
* **server:** State maxWidth and maxHeight on a size-restricted info.json ([fa7a298](https://github.com/dasch-swiss/sipi/commit/fa7a298dea7f64e388487175e84fc399b9644328))


### Code Refactoring

* **ffi:** Probe image dims under the decode deadline ([8ab2466](https://github.com/dasch-swiss/sipi/commit/8ab2466efeaa904453fe0d062ee068b292f8716d))
* **format_handlers:** Scale codec reads through scale_with ([df9c6dd](https://github.com/dasch-swiss/sipi/commit/df9c6dd325a8f5c04b8649743c06e2f279ee1dc3))
* **iiifparser:** Add size_never_reduces for the restricted-view refusal ([cb73340](https://github.com/dasch-swiss/sipi/commit/cb733407178b9ef5dd0ce8b2d99edab0ca4aac6f))
* **image_processing:** Add scale_with, the one scaling-method dispatch ([a9203c4](https://github.com/dasch-swiss/sipi/commit/a9203c4e5f9dedd4447ce58ddeeebfef5dbb782d))
* **image:** Add scaling_method_for, the codec-to-scaling-method mapping ([be14210](https://github.com/dasch-swiss/sipi/commit/be1421006bbf51f26b9596f76e9e7a8a5f544fc5))
* **metadata:** Add the size-field rewrite and preview removal edits ([331cdd9](https://github.com/dasch-swiss/sipi/commit/331cdd94e729f5c7a01f54efa3b490cb06a2784c))
* **metadata:** Fit EXIF and XMP blocks into one JPEG marker segment ([dd0fe9c](https://github.com/dasch-swiss/sipi/commit/dd0fe9c77c1d68702c5570a6a3b6bfd24698b484))


### Documentation

* **bazel:** Describe the compile database workflow for clangd and clang-tidy ([4065f37](https://github.com/dasch-swiss/sipi/commit/4065f37776b8c98d3d7b69c8c408caa9074a0ed7))
* **docs:** Make the agent reader the leading principle for commit cleanup ([320628a](https://github.com/dasch-swiss/sipi/commit/320628a5858afe03749bdcb841e1837d5f02d621))
* **docs:** Note the Vitrinli rename and how to list its Linear issues ([724ce64](https://github.com/dasch-swiss/sipi/commit/724ce64c24886c2f790b518e9801dd6a24578d2f))
* **docs:** Record the restricted-output metadata rewrite ([d035bbe](https://github.com/dasch-swiss/sipi/commit/d035bbe14196b198e68e9a22f545e31c6eeca26e))
* **docs:** Record the restricted-view coordinate space in ADR-0028 ([fdabe98](https://github.com/dasch-swiss/sipi/commit/fdabe989cd2229515b2fc6e1e97efc0a9a575728))
* **format_handlers:** Record the JPEG metadata fit policy ([2cf1b10](https://github.com/dasch-swiss/sipi/commit/2cf1b109d1c05c1f18e08fd2dd2156b59a5d0bd2))
* **server:** List the info.json and knora.json CORS cases in the testing strategy ([2c74595](https://github.com/dasch-swiss/sipi/commit/2c74595a2d9a01d76acaf7fa73271b3c405040a8))
* **specs:** Add the Bazel toolchain upgrade plan and its journal ([cad0cc0](https://github.com/dasch-swiss/sipi/commit/cad0cc00fca14d5d34ced33e432fbffc446ec889))
* **specs:** Add the Vitrinli Todo bug backlog plan and its journal ([6dc1289](https://github.com/dasch-swiss/sipi/commit/6dc12896eb3ed7712f34e9aa8527d54770629344))


### Tests

* **format_handlers:** Pin the FlashEnergy EXIF type in the TIFF tag table ([64e0351](https://github.com/dasch-swiss/sipi/commit/64e0351165e94b8921f7155f2e1a21f3ddfbdbbb))


### Build System

* **bazel:** Generate compile_commands.json for clangd and clang-tidy ([c036158](https://github.com/dasch-swiss/sipi/commit/c036158ae3a4f2baaeb76a7f065d2d4b1a9cfacb))
* **bazel:** Make the macOS Rust links warning-clean ([2272102](https://github.com/dasch-swiss/sipi/commit/227210229986a8c330a4bb950df8fe785986422f))
* **bazel:** Upgrade Bazel to 9.3.0 ([a46523c](https://github.com/dasch-swiss/sipi/commit/a46523c2dcbfbe9cfb8d503b29dfc99ed6347d20))
* **bazel:** Upgrade hermetic-llvm to 0.8.25 ([e138f0b](https://github.com/dasch-swiss/sipi/commit/e138f0b15fd609fa888991d9895853f65e01631a))
* **bazel:** Upgrade LLVM to 23.1.2 ([0ce24da](https://github.com/dasch-swiss/sipi/commit/0ce24dab29e482b8f5272997d3f00ff52e7842a2))
* **bazel:** Upgrade rules_rust to 0.74.0 ([923cfd5](https://github.com/dasch-swiss/sipi/commit/923cfd5c4da65c8178845bd6fead3da42baf1fcc))
* **bazel:** Upgrade Rust to 1.98.0 ([db27f34](https://github.com/dasch-swiss/sipi/commit/db27f34323638c3968eead09d5e9d0f8a9477382))

## [9.1.1](https://github.com/dasch-swiss/sipi/compare/v9.1.0...v9.1.1) (2026-09-29)


### Bug Fixes

* **deps:** Raise rustls to 0.23.45, repair the dev-shell hook, run audit locally ([7a8e42c](https://github.com/dasch-swiss/sipi/commit/7a8e42cb389dc007ab218bef51fdf80028bfa50c))
* **format_handlers:** Cap Kakadu worker threads at the thread group's limit ([25305f3](https://github.com/dasch-swiss/sipi/commit/25305f31c0327891ca8e5faa5aec7e9347128d9b))
* **format_handlers:** Keep zero-denominator EXIF rationals away from libtiff ([6dddc75](https://github.com/dasch-swiss/sipi/commit/6dddc75fa86a46a41ccfe48d0ff0e89c631a11e2))
* **format_handlers:** Stop a failed PNG or JPEG write from skipping destructors ([7d2e71f](https://github.com/dasch-swiss/sipi/commit/7d2e71ffd2e89a06efa03e678a4494c00fa957de))
* **iiifparser:** Clamp a percent size before converting it to an integer ([f729db4](https://github.com/dasch-swiss/sipi/commit/f729db4ec79dd2afe982249b156b23d5faea2cc2))
* **metadata:** Initialise the white point luminance in synthesised RGB profiles ([b2a28e7](https://github.com/dasch-swiss/sipi/commit/b2a28e75cbff123257075c29efab8c218cb478e4))
* **metadata:** Reject TIFF colorimetry that no ICC profile can represent ([cd03ddd](https://github.com/dasch-swiss/sipi/commit/cd03ddd722e936d1dba7d0e46a5f2c6ab1a4975f))
* **util:** Close the per-thread libmagic handle on thread exit ([ff43648](https://github.com/dasch-swiss/sipi/commit/ff43648d84f5fe02f069f2b9071f6850a04b8260))


### Documentation

* **docs:** Adopt the shared comment convention (DEV-7123) ([a2db7a6](https://github.com/dasch-swiss/sipi/commit/a2db7a672ffeab28190e39f283bee8c71efc3538))
* **server,cli:** Trim the Rust shell's comments to the convention (DEV-7123) ([a61fb63](https://github.com/dasch-swiss/sipi/commit/a61fb634b8098cd852adae2de6d9712103115aef))
* **specs:** Add the libmagic per-decode leak plan and its journal ([dc1dcc9](https://github.com/dasch-swiss/sipi/commit/dc1dcc9fe8c8db446c2e3f9d61c7fa35e3f679c7))
* **specs:** Close out the asset-access plan and its journals ([9f9f8f2](https://github.com/dasch-swiss/sipi/commit/9f9f8f22f8388d29d59a0960d9f157bc8d806f80))


### Miscellaneous Chores

* **ci:** Make the sanitizer job fail on leaks and undefined behaviour ([a1b5c5e](https://github.com/dasch-swiss/sipi/commit/a1b5c5ec42b6ca73db9ab718ae27e6fdbdd6faef))
* **ci:** Restore AddressSanitizer thread tracking in the Rust shell ([533d27a](https://github.com/dasch-swiss/sipi/commit/533d27aa130d8e858733de353cb725eba01ad521))

## [9.1.0](https://github.com/dasch-swiss/sipi/compare/v9.0.1...v9.1.0) (2026-09-12)


### Features

* **server,observability:** Add the stream permission type ([c17d413](https://github.com/dasch-swiss/sipi/commit/c17d41362f40c0671139d2572373725dee7ea8ad))


### Documentation

* **docs:** Document the stream permission type ([a9be587](https://github.com/dasch-swiss/sipi/commit/a9be58781f634890146fe4fca2601360e5d1cc98))
* **docs:** Record the VRE's Original/Derivative vocabulary ([abdd939](https://github.com/dasch-swiss/sipi/commit/abdd939dfc3cfa57fb04931ab486b4f58af14665))
* **docs:** Register the Permission vocabulary in the architecture map ([93c0a7d](https://github.com/dasch-swiss/sipi/commit/93c0a7de41760ec61fb1f2d8bf2715fdc6186e0f))
* **specs:** Correct the asset-access plan's release-coupling model ([ab9e84d](https://github.com/dasch-swiss/sipi/commit/ab9e84d129d47cdf070f1d5b2379d1924d5040fd))
* **specs:** Plan asset access as a first-class decision ([c03094e](https://github.com/dasch-swiss/sipi/commit/c03094e30c842777c4ecbb681ed5b2b69ce2bb46))
* **specs:** Tick wave-2 Phase 12 (Linear reconciliation) as done ([05e0083](https://github.com/dasch-swiss/sipi/commit/05e0083fc09115bf0c561aff131b78f18e10e9fe))


### Tests

* **e2e:** Give the public-hosts server its own cache directory ([8505424](https://github.com/dasch-swiss/sipi/commit/85054243e2eaa24e6a9ad061d454e7b23cee2bcc))
* **format_handlers:** Size the codec fuzz decode budget for the ASan -O0 pass ([8414705](https://github.com/dasch-swiss/sipi/commit/8414705fe9bc68f720a1e045f75e66708b46cd2d))


### Miscellaneous Chores

* **ci:** Correct the stale Darwin libFuzzer note in fuzz.yml ([f1cb142](https://github.com/dasch-swiss/sipi/commit/f1cb1422bfe5703c21234620238d8bebeeb39a90))

## [9.0.1](https://github.com/dasch-swiss/sipi/compare/v9.0.0...v9.0.1) (2026-09-08)


### Bug Fixes

* **cli:** Bound offline-verb decodes with the seam's decode deadline ([ac3bf78](https://github.com/dasch-swiss/sipi/commit/ac3bf789106ebf747a790711deb1a8f304158c2f))
* **format_handlers:** Destroy the Kakadu codestream before the JPX target on write errors ([6cd46ff](https://github.com/dasch-swiss/sipi/commit/6cd46ff0b88027b7f9feb5e2709b0aaf105611f8))
* **format_handlers:** Reject tiled TIFFs whose TIFFTileSize is smaller than the tile geometry ([1de76df](https://github.com/dasch-swiss/sipi/commit/1de76df9d425e9c231ad44d2875745f80800cc8a))
* **metadata:** Store EXIF ASCII tags as their characters, not the string object ([d9c7704](https://github.com/dasch-swiss/sipi/commit/d9c7704de2b341d40e7c5cd982966309f8e6cc6c))


### Documentation

* **learnings:** A red fuzz leg reports one finding at a time — fix, re-dispatch, expect the next ([5ecc9a2](https://github.com/dasch-swiss/sipi/commit/5ecc9a203367459acb0a88dc7acd02c3c85ab55f))
* **learnings:** A seam mitigation does not green a codec-level fuzz leg — tolerate the known class in fork mode ([cf1673d](https://github.com/dasch-swiss/sipi/commit/cf1673ddbb1217dd2e7e928bd911398d763ee1d9))
* **learnings:** An ASan container-overflow from a fuzz binary is a link artifact until a non-fuzz ASan build reproduces it ([9eb78db](https://github.com/dasch-swiss/sipi/commit/9eb78dbc58538e4046b1cec07c83a327d1131eb9))
* **learnings:** Destroy a Kakadu codestream before its target unwinds; never read a class type through &val/sizeof(T) ([cbca58f](https://github.com/dasch-swiss/sipi/commit/cbca58f31292db6540b53a863917f1a6bc23aaeb))


### Tests

* **format_handlers:** Cap the round-trip fuzz harness re-encode at 16 MiB of pixels ([499523f](https://github.com/dasch-swiss/sipi/commit/499523fd7744c29626d333053e75c5a54810f21f))


### Build System

* **deps:** Exclude Kakadu alignment and lcms2 function UBSan checks ([7ac5f43](https://github.com/dasch-swiss/sipi/commit/7ac5f43ec48dc449cadfbed4a7f2ccc5d31911ac))


### Miscellaneous Chores

* **ci:** Keep the nightly fuzz legs green on known non-defects ([8bb3b43](https://github.com/dasch-swiss/sipi/commit/8bb3b431bf6975c6f635e668903ae55d4989f03d))

## [9.0.0](https://github.com/dasch-swiss/sipi/compare/v8.0.0...v9.0.0) (2026-09-06)


### ⚠ BREAKING CHANGES

* **cli:** the `--adminuser` and `--adminpasswd` CLI flags and the `SIPI_ADMINUSER` / `SIPI_ADMINPASSWD` environment variables are removed. They configured nothing; deployments passing them must drop them.
* **throttling:** generic deployments that relied on the basic default now enforce the advanced tier and may answer 503/413 where they previously admitted large requests. Set SIPI_ADMISSION_MODE=basic to restore the prior shadow-only behavior.

### Features

* **scripting:** Add a constant-time compare binding for credential checks ([8ca249e](https://github.com/dasch-swiss/sipi/commit/8ca249e6a3a5c309b1a3983c68ba38ae85eaf900))
* **throttling:** Default admission_mode to advanced ([dde8070](https://github.com/dasch-swiss/sipi/commit/dde8070bab8acdf7c1d5426771c78eee59d76c9c))


### Bug Fixes

* **cache:** Clamp cache_used_bytes subtractions against underflow ([aeb8b8b](https://github.com/dasch-swiss/sipi/commit/aeb8b8bee34ab41d1a163771d830b47c9d300d2b))
* **cache:** Harden the cache index format and reads against forgery ([413cacc](https://github.com/dasch-swiss/sipi/commit/413cacc585633640ce5026674a084fad005adc2b))
* **ffi:** Bound wedged JP2 decodes with a seam deadline (DEV-7080) ([13c4d0e](https://github.com/dasch-swiss/sipi/commit/13c4d0e0846ad81bfd2014974c1c5ce1542e4766))
* **ffi:** Cap restricted views by sampling factor across regions ([6782442](https://github.com/dasch-swiss/sipi/commit/678244213aa6076e16cafb255a6df6c60db90f8c))
* **ffi:** Charge the memory budget for Lua SipiImage.new decodes ([1c85ddf](https://github.com/dasch-swiss/sipi/commit/1c85ddf6af3ba7382ef34f31119dddfe359f59cc))
* **ffi:** Length-cap the Range header before the regex ([c3c5ef1](https://github.com/dasch-swiss/sipi/commit/c3c5ef150d2dc4f6baf8dd231906ccacf8cde633))
* **ffi:** Let Lua SipiImage.new decode without an installed engine context ([f3e41ed](https://github.com/dasch-swiss/sipi/commit/f3e41edae8800d487ffedb527ee7befd53992acf))
* **ffi:** Redact paths and drop the request-uri tag from error events ([2b502ad](https://github.com/dasch-swiss/sipi/commit/2b502ad8c410039f33fd9c98cca53f56b7807533))
* **ffi:** Refuse restrict decisions that do not restrict ([7f9785b](https://github.com/dasch-swiss/sipi/commit/7f9785bd434079d913564240f468e0f6838ccf8d))
* **ffi:** Reset the connection when an encode fails after the head is sent ([1dbcec5](https://github.com/dasch-swiss/sipi/commit/1dbcec568e5c38504faa896c5f92cc974255a0fc))
* **format_handlers:** Bound JPEG marker lengths and fix IPTC APP13 length ([29f7798](https://github.com/dasch-swiss/sipi/commit/29f779865033d8d0cdf360f319cb77574c19b898))
* **format_handlers:** Correct TIFF planar-separate ROI destination offsets ([3f638c9](https://github.com/dasch-swiss/sipi/commit/3f638c900d7f901cf590f40092a945a5ef1f1001))
* **format_handlers:** Guard trusted dimensions and empty ICC in codec paths ([49d2313](https://github.com/dasch-swiss/sipi/commit/49d2313c6824b63e1dcb8fe661703f04c823117c))
* **format_handlers:** Read each plane of a tiled planar-separate TIFF ([ef3b46d](https://github.com/dasch-swiss/sipi/commit/ef3b46d4339183711b3e6ee238f821c6af35d91a))
* **format_handlers:** Reject TIFF scanlines smaller than the decoded line ([db96c4d](https://github.com/dasch-swiss/sipi/commit/db96c4d6a8e94508f1dffbf185ce5f1b784fb85f))
* **format_handlers:** Size JP2 stripe_heights to the component count ([66f6c2c](https://github.com/dasch-swiss/sipi/commit/66f6c2c77140ff90efb76ede8a18ee6482d9fd26))
* **format_handlers:** Validate JP2 metadata box lengths and reads ([1ddfd95](https://github.com/dasch-swiss/sipi/commit/1ddfd9589b70133be51af182f2e7d83edd4f26d8))
* **iiifparser:** Bound rotation, region, and size-pct against out-of-range input ([6751d58](https://github.com/dasch-swiss/sipi/commit/6751d58eb79dbf755d9f7424841b6f9d46e2f227))
* **image_processing:** Avoid divide-by-zero in subtract on identical inputs ([74e00fd](https://github.com/dasch-swiss/sipi/commit/74e00fd51aaf5f83a2ec402496fadf6e4ed80b45))
* **image_processing:** Guard convertYCC2RGB channel count and 16-bit midpoint ([07c6dc2](https://github.com/dasch-swiss/sipi/commit/07c6dc294544fc559539899c92642f845284c2c7))
* **metadata:** Compute base64 decode length without integer underflow ([8492b89](https://github.com/dasch-swiss/sipi/commit/8492b89f9b6c6c58feae42152d3b2046ffd6d426))
* **scripting:** Default response cookies to HttpOnly and SameSite=Lax ([3a9e282](https://github.com/dasch-swiss/sipi/commit/3a9e282ece7464c0a4048510b4ebb5170ce3ce55))
* **scripting:** Delete the token.lua postMessage relay and its route ([594e760](https://github.com/dasch-swiss/sipi/commit/594e760ccf3eb85248cc536ab52bbdd4e037172f))
* **scripting:** Harden Lua print, mkdir mode, and json depth ([f3a0e21](https://github.com/dasch-swiss/sipi/commit/f3a0e211ed096b85a9fb910b543b3c67296c2dfc))
* **scripting:** Require a token and a uuid62 basename for the upload example ([7b92dbb](https://github.com/dasch-swiss/sipi/commit/7b92dbb9f46af9563c65528c8ce89c6552c772c8))
* **server:** Admit docroot static serving and cache the libmagic database ([1f79fa5](https://github.com/dasch-swiss/sipi/commit/1f79fa52fe17f00a99373b4d4e11e7123b1f2e30))
* **server:** Answer 404 for image-path traversal and docroot dotfiles ([553f2cf](https://github.com/dasch-swiss/sipi/commit/553f2cfbc684f5b6e70a3f142b1f07712f2cb680))
* **server:** Answer 408 when a request handler exceeds its timeout ([255a288](https://github.com/dasch-swiss/sipi/commit/255a2881303542ecd2947dc369a5ea30d198c6b0))
* **server:** Bound Lua-route request bodies by size and time before admission ([b6b0984](https://github.com/dasch-swiss/sipi/commit/b6b0984f5d2f4a3adf840458a172efdfc749772b))
* **server:** Gate bare-restrict info.json and knora.json behind a denial ([996a0f1](https://github.com/dasch-swiss/sipi/commit/996a0f1be9c69b8064837a8dd158c1e3546fe6da))
* **server:** Refuse startup on an unsafe jwt_secret and drop the shipped default ([92d54c7](https://github.com/dasch-swiss/sipi/commit/92d54c7c31a385bf2bc29ad7d787b47b13b49548))
* **server:** Refuse startup when docroot overlaps a writable or script root ([348ec28](https://github.com/dasch-swiss/sipi/commit/348ec28ffe4a6b0cb8ac3ebccc247999460648ed))
* **server:** Stop info.json and knora.json leaking restricted dimensions ([7f9adda](https://github.com/dasch-swiss/sipi/commit/7f9addac882ae5ffc8d70ad87253b71d2cba1208))
* **server:** Structurally enforce the query-free preflight cache key ([3a19452](https://github.com/dasch-swiss/sipi/commit/3a19452e1781f967a950852dd8a9080970061647))
* **server:** Validate the forwarded host against a SIPI_PUBLIC_HOSTS allowlist ([1baaa1a](https://github.com/dasch-swiss/sipi/commit/1baaa1a3dd78c0d52a70b84d7381315776e924b3))


### Code Refactoring

* **cli:** Remove the dead adminuser and adminpasswd surface ([b3afa68](https://github.com/dasch-swiss/sipi/commit/b3afa685bc3b71c0df6a8d79afddf914f6c4928e))
* **ffi:** Drop the unused client_ip field from the serve seam ([4648cf0](https://github.com/dasch-swiss/sipi/commit/4648cf0f3d847ccbf24b2b391d2eb0f5f95f19c1))
* **ffi:** Route both decode paths through one guarded helper ([1c46c1c](https://github.com/dasch-swiss/sipi/commit/1c46c1c2f70fd874c8e4c728bceb022a6a38f871))


### Documentation

* **adr:** Add ADR-0026 and the engine-seam extraction plan ([62fbc04](https://github.com/dasch-swiss/sipi/commit/62fbc044d0c21acb4ae5233624f18680c909a3cb))
* **adr:** Record the per-client fairness ruling in ADR-0022 ([48f60ef](https://github.com/dasch-swiss/sipi/commit/48f60ef803042207cd22cd9ef558a7e86f1c0ce0))
* **adr:** Scrub stale paths and fix status metadata in active ADRs ([a088db2](https://github.com/dasch-swiss/sipi/commit/a088db2e417fe7ced6a3ea6e98973b6a7ea0659c))
* **cli:** Reconcile config, CLI help, and domain docs with wave-2 behavior ([94267f4](https://github.com/dasch-swiss/sipi/commit/94267f45a4b1f65150f58a3f2619433edc251812))
* **docs:** Refresh ARCH-MAP entries after the wave-2 branch ([460deb0](https://github.com/dasch-swiss/sipi/commit/460deb09a1cba4bc0c28c3b8617ea914eb46b19c))
* **learnings:** A supervised subagent must return at a long build, not park on it ([409c92b](https://github.com/dasch-swiss/sipi/commit/409c92ba6c9a47605e801d5ad9cd43fc34331f4f))
* **learnings:** A timeout/slowloris e2e must be bounded on server, client, teardown, harness ([ee5ffc2](https://github.com/dasch-swiss/sipi/commit/ee5ffc2fa5b663bf6f3d48f948af66aeb9a8fbdc))
* **learnings:** Capture the wave-2 goose chase — primary evidence over plausible hypotheses ([51f9d4e](https://github.com/dasch-swiss/sipi/commit/51f9d4e3966db1aa1e0c36f1b227563439d2ad72))
* **learnings:** Verify a duplication finding with a set-diff — the FFI mirrors are disjoint ([a967f50](https://github.com/dasch-swiss/sipi/commit/a967f50079995f3920988cb6d84b72999bbf0eed))
* **specs:** Add SIPI security hardening wave 2 plan ([cceab50](https://github.com/dasch-swiss/sipi/commit/cceab505ea89a7ec554b95cd1a3a27e7ac749ebc))
* **specs:** Flatten spec folders into flat date-numbered files ([ec1b690](https://github.com/dasch-swiss/sipi/commit/ec1b690bd9ab07ad2b2f808bb7015426d607200d))
* **specs:** Record the JP2 decode watchdog mechanism (DEV-7080) ([6bf8ff6](https://github.com/dasch-swiss/sipi/commit/6bf8ff636832817aef01419374dc6353e6e3c5ab))
* **specs:** Record the wave-2 CI stabilization journal ([075e5aa](https://github.com/dasch-swiss/sipi/commit/075e5aa55d124868b5ce8c14c0c6151e22f8fefd))


### Tests

* **e2e:** Isolate the cache dir of concurrent resource_limits servers ([ae55eaa](https://github.com/dasch-swiss/sipi/commit/ae55eaad31eda5135e3a4c17d8a038c8502b42e3))
* **e2e:** Read the TIFF-JPEG body before deleting its source fixture ([32ff41c](https://github.com/dasch-swiss/sipi/commit/32ff41c3b91ec6207c6ac619bf86608fe48a639c))
* **fuzz:** Add codec round-trip encode fuzz targets ([37c9ee4](https://github.com/dasch-swiss/sipi/commit/37c9ee4695cace3f5098c504da4fe9aeadc9855f))
* **fuzz:** Disable ASan container-overflow check on the parser leg ([bd056d8](https://github.com/dasch-swiss/sipi/commit/bd056d8a4f82ec5fc8542917516782bc01e6a1c9))
* **fuzz:** Drive the region/size-aware decode path from a header prefix ([f93595d](https://github.com/dasch-swiss/sipi/commit/f93595d16a419f84e35469f7f29ad5954bd17e9b))
* **fuzz:** Lower the j2k decode fuzz max_len to raise throughput ([80e883e](https://github.com/dasch-swiss/sipi/commit/80e883e926a74adcf48bb107bdb796b35d3dfed6))
* **fuzz:** Pin the DEV-7080 JP2 decode-hang reproducers ([9a5fe00](https://github.com/dasch-swiss/sipi/commit/9a5fe00044cdf7b4cf926d14818673bc51f41514))
* **fuzz:** Silence libtiff warning flood and cap fuzz logs ([84ca70d](https://github.com/dasch-swiss/sipi/commit/84ca70df5f3cba45cadc95c80a152af1e2f1957c))


### Build System

* **deps:** Bump curl to 8.21.0.bcr.1 ([7edda83](https://github.com/dasch-swiss/sipi/commit/7edda8394c253c417ac3d411f116cbcd34bf67ba))
* **deps:** Bump exiv2 to 0.28.9 ([5951a1f](https://github.com/dasch-swiss/sipi/commit/5951a1fa7d1d55edb09b60202f5a0209a9e9fbc2))
* **deps:** Bump lcms2 to 2.19.1 ([c75e95d](https://github.com/dasch-swiss/sipi/commit/c75e95dff15f708fa72762b8302538bfb76bfe6c))
* **deps:** Bump libpng to 1.6.58 ([8c6ef37](https://github.com/dasch-swiss/sipi/commit/8c6ef377d66656c5da5d0aa1179fdbf02d1ed987))
* **deps:** Bump libtiff to 4.7.2 ([fe63eb2](https://github.com/dasch-swiss/sipi/commit/fe63eb2c89ac8bee1662dba180f08f74723cfbbf))
* **deps:** Vendor libexpat 2.8.4 as a native cc_library ([daa642a](https://github.com/dasch-swiss/sipi/commit/daa642af91e1b9b8d22075b04c30678b6af2b0e6))


### Miscellaneous Chores

* **ci:** Audit crate advisories over a checked-in lockfile ([3d1afcf](https://github.com/dasch-swiss/sipi/commit/3d1afcf0129bcd1f0b68e9dab35c6660cd43c525))
* **ci:** Harden the GitHub Actions supply-chain posture ([df8e9a6](https://github.com/dasch-swiss/sipi/commit/df8e9a61b7db72727ad37ff3df02d651216344bd))

## [8.0.0](https://github.com/dasch-swiss/sipi/compare/v7.0.0...v8.0.0) (2026-08-31)


### ⚠ BREAKING CHANGES

* **format_handlers:** a file carrying unparseable embedded metadata that previously decoded (with the bad metadata silently discarded) is now rejected.

### Features

* **image_processing:** Apply watermarks to 16-bit images ([0b283bb](https://github.com/dasch-swiss/sipi/commit/0b283bbda1d80a166b2ce98e6976ac7cf3e181b8))
* **util:** Add checked-multiply and dimension-validation helpers ([b206307](https://github.com/dasch-swiss/sipi/commit/b2063075361f3bade8ea81395c10db636cd8b613))


### Bug Fixes

* **e2e:** Accept both abort orderings in the post-commit abort tests ([c22017d](https://github.com/dasch-swiss/sipi/commit/c22017d7c071d0b757886585aa517ffc779f8609))
* **error:** Answer a request for an impossible size with 400, not 500 ([a592b0b](https://github.com/dasch-swiss/sipi/commit/a592b0b14bd0f79d890f7b45b19c0a90476946b7))
* **error:** Keep the opening quote when redacting paths from error text ([4e54436](https://github.com/dasch-swiss/sipi/commit/4e5443649ba2bec2116a7898d0b02d3d2d466bec))
* **format_handlers:** Bound the raw-profile reservation to the available data ([f693e5d](https://github.com/dasch-swiss/sipi/commit/f693e5d1befb84567d66cd450d787e56a5238f6f))
* **format_handlers:** Decode JPEG-compressed TIFFs ([aada66e](https://github.com/dasch-swiss/sipi/commit/aada66e981712c5ad1a32bd8210e35ffef73eca4))
* **format_handlers:** Make the JPEG setjmp landing blocks read defined values ([beb1357](https://github.com/dasch-swiss/sipi/commit/beb13576ae93dc3be5434f9887de714aee437350))
* **format_handlers:** Refuse a file whose embedded metadata does not parse ([e6f1788](https://github.com/dasch-swiss/sipi/commit/e6f17885a650db14db249ddec0a66f0430dcc6f4))
* **format_handlers:** Refuse a PNG whose EXIF chunk does not parse ([4f0972f](https://github.com/dasch-swiss/sipi/commit/4f0972f6bd4c0e1daa67076ba6bca9fd9c9ded39))
* **format_handlers:** Refuse a TIFF whose colour tags cannot form an ICC profile ([c6674ac](https://github.com/dasch-swiss/sipi/commit/c6674ac2b9d30826ade9ce843be97cf0abbc3aa2))
* **format_handlers:** Release the codec library structs on the JPEG and PNG error paths ([69b8183](https://github.com/dasch-swiss/sipi/commit/69b818342850154b1f6d67b5227f4b62f98d9ec5))
* **format_handlers:** Release the PNG read structs on the metadata-failure arms ([3f4edff](https://github.com/dasch-swiss/sipi/commit/3f4edff6c727db27d6ab038b9f5aeb15aa7038fc))
* **format_handlers:** Release the PNG write structs on every exit path ([23db0e9](https://github.com/dasch-swiss/sipi/commit/23db0e9068700dbc5d963f6f9218e28b3acb842e))
* **format_handlers:** Report a J2K stripe-decode failure as a decode error ([5e14b5e](https://github.com/dasch-swiss/sipi/commit/5e14b5e05064c04ffd918af32402f8315852a4fa))
* **format_handlers:** Report libtiff's errors and warnings instead of dropping them ([ee12d37](https://github.com/dasch-swiss/sipi/commit/ee12d37281b1a929187d0b96263b986afb9b356d))
* **format_handlers:** Stop truncating embedded PNG metadata ([dc12a6d](https://github.com/dasch-swiss/sipi/commit/dc12a6d186b96d741e001520999ff5aea3221b19))
* **formats:** Bound JPEG marker-parsing over-reads ([176064e](https://github.com/dasch-swiss/sipi/commit/176064e965a90876a04ca90e4f4bf5951cbeb194))
* **formats:** Bound parse_photoshop pointer advances past marker end ([ade2535](https://github.com/dasch-swiss/sipi/commit/ade2535dade32a7f3f179509d3a89ac8882dd8c9))
* **formats:** Guard J2K palette expansion against overflow and OOB reads ([dd1197d](https://github.com/dasch-swiss/sipi/commit/dd1197dd8ba6c8e8d3539377b8c57bb69b4dac23))
* **formats:** TIFF buffer and libtiff-field bugs ([262b788](https://github.com/dasch-swiss/sipi/commit/262b788831167aadbe14e830dd3851ca6e9e8553))
* **formats:** Widen PNG 16-bit byte-swap loop counter to size_t ([a406034](https://github.com/dasch-swiss/sipi/commit/a40603435e23c17e7795cc342ed5decf02e724b5))
* **image_processing:** Reject a crop the image's bit depth cannot support ([7d07189](https://github.com/dasch-swiss/sipi/commit/7d071897ca0d2a898cac457684177cb4d0bc982e))
* **image_processing:** Reject a scale to a one-pixel axis instead of serving the source ([5c8140a](https://github.com/dasch-swiss/sipi/commit/5c8140a43f062c072a1c4403a5f3a0aea47dc18e))
* **image_processing:** Stop serving uncropped images when the crop fails ([1d5ab65](https://github.com/dasch-swiss/sipi/commit/1d5ab65a87dcff94d3d95c8e11d592d7be41b194))
* **image:** Correct channel-count buffer sizing and indexing ([7cc208e](https://github.com/dasch-swiss/sipi/commit/7cc208ec292500a1479ef917110db5354379d285))
* **image:** Guard pixel-buffer allocations against integer overflow ([5631934](https://github.com/dasch-swiss/sipi/commit/5631934e6cbd5c2c19afa5c8fa9b7549430e582b))
* **logging:** Declare log_vformat with the signature it is defined with ([18d074e](https://github.com/dasch-swiss/sipi/commit/18d074e0e9286f0e21160c916243c877889d07f8))
* **metadata:** Classify a description-less ICC profile without reading past its buffer ([f8ebd78](https://github.com/dasch-swiss/sipi/commit/f8ebd788715346fb0289170f2ed4c8c7054b47e3))
* **metadata:** Read a profile's optional info tags without running off the buffer ([32032aa](https://github.com/dasch-swiss/sipi/commit/32032aadcbbdfed5bb3ba867c2e4843b0e443d45))
* **observability:** Stop leaking source paths in client error messages ([1b022c6](https://github.com/dasch-swiss/sipi/commit/1b022c6a4d57eea37786f9c0a584a14dda963a04))
* **server-rs:** Restrict CORS to a configured origin allowlist ([411a03b](https://github.com/dasch-swiss/sipi/commit/411a03b5ecf64adf3dfdd185feeb732f4328642b))


### Code Refactoring

* **bazel:** Organize sources into cpp/ and rust/ subfolders and colocate the tests ([3aaae3d](https://github.com/dasch-swiss/sipi/commit/3aaae3dfe0dcbc7acbe7fbbe66680427c6a37b1d))
* **error:** Add the value-error foundation and seam conversion ([47bf70d](https://github.com/dasch-swiss/sipi/commit/47bf70d6cd2e8773f664510a983ef8b27d91f26c))
* **error:** Expose the value error's message, errno and source location ([b928219](https://github.com/dasch-swiss/sipi/commit/b928219e54b078d06e8e0644afc28b258d9d764f))
* **ffi:** Drop the shape guard the probe can no longer trip ([7efc17c](https://github.com/dasch-swiss/sipi/commit/7efc17c7a1f2d27f86d002e9c8612f02cc8650f5))
* **format_handlers:** Carry libtiff's diagnostics in the TIFF reader's errors ([58a35ed](https://github.com/dasch-swiss/sipi/commit/58a35edb5ffad5e42d4779f871c240d7dc68789c))
* **format_handlers:** Convert the J2K handler internals to value errors ([48e02c8](https://github.com/dasch-swiss/sipi/commit/48e02c89fd5b334e174245731f43d09d6ae1f986))
* **format_handlers:** Convert the JPEG handler internals to value errors ([8ae85c6](https://github.com/dasch-swiss/sipi/commit/8ae85c6260b985381fb47457ac9efbb734dfabcb))
* **format_handlers:** Convert the PNG handler internals to value errors ([b240d6d](https://github.com/dasch-swiss/sipi/commit/b240d6dd67973c375cc35f7152876a99606001b7))
* **format_handlers:** Convert the TIFF handler internals to value errors ([18e4519](https://github.com/dasch-swiss/sipi/commit/18e4519c9c8508b11549acc5c4b75e8a61214103))
* **format_handlers:** Delete the dead separate-to-contig member ([7bbf86d](https://github.com/dasch-swiss/sipi/commit/7bbf86ddc184e503a7b211d2ef14b6784ffb5511))
* **format_handlers:** Drop the catch around an XMP construction that cannot throw ([7fef718](https://github.com/dasch-swiss/sipi/commit/7fef718c3a8f3183ba91386450008859b7930507))
* **format_handlers:** Drop the redundant geometry write on the CMYK-to-PNG path ([0d94c0c](https://github.com/dasch-swiss/sipi/commit/0d94c0c79f8cdb4c8fca2d95ded517042976c1e5))
* **format_handlers:** Remove the Image friendships and make the APP14 transform decode-local ([7cf4b51](https://github.com/dasch-swiss/sipi/commit/7cf4b5136fbd3faf51672178b2df79514e9f0407))
* **format_handlers:** Rename the formats package and split its sources ([421de2b](https://github.com/dasch-swiss/sipi/commit/421de2b1b0bde4a2acd2bba254b0d54ac982ef94))
* **format_handlers:** Retire the JPEG metadata catch blocks ([8aaf3f1](https://github.com/dasch-swiss/sipi/commit/8aaf3f1b1b77956fbfadef50b992ed4e8b4cef0c))
* **format_handlers:** Return the decode-dimension guard's failures as values ([1ce443c](https://github.com/dasch-swiss/sipi/commit/1ce443c69ccecfea9270d970f01038acb7c16935))
* **format_handlers:** Return the watermark reader's failures as values ([686e742](https://github.com/dasch-swiss/sipi/commit/686e7420f4a429a4430143dadc17b5bf2e108930))
* **image_processing:** Convert the processing functions to Result returns ([1f6953e](https://github.com/dasch-swiss/sipi/commit/1f6953e00d5f4d28550365ebfc2eb58610a0efbd))
* **image_processing:** Drop the unused arithmetic operators and value-error the difference ([8c9d213](https://github.com/dasch-swiss/sipi/commit/8c9d213431a1e46b4720b3989ecf990fab0b71ff))
* **image:** Add the public mutator surface and extract the image_processing free functions ([486705e](https://github.com/dasch-swiss/sipi/commit/486705e6b8abf3b2e68d9cab3a5812c244bf9dec))
* **image:** Dissolve //src:engine into the //src/image package ([7b931bc](https://github.com/dasch-swiss/sipi/commit/7b931bcb23c1bc59467710dd4d812f57c0cf5540))
* **image:** Dissolve the SipiImage hub into image_processing, cache, and error and rehome the strays ([5d14c9b](https://github.com/dasch-swiss/sipi/commit/5d14c9bfa85d0eb8c104c2a9d03671f7efbdbe4a))
* **image:** Return image I/O failures as values across the SipiIO vtable ([f6370f7](https://github.com/dasch-swiss/sipi/commit/f6370f78329600571912a8a716e95f8f120aebfa))
* **image:** Return the image read and write failures as values ([06c4507](https://github.com/dasch-swiss/sipi/commit/06c4507ce404c406c3f239c11476a5098b472b46))
* **image:** Return the shape probe's failures as values ([6752d86](https://github.com/dasch-swiss/sipi/commit/6752d86dcdd7cc1e6e62b6e48cb90a992c51ce2c))
* **image:** State the image layer's exception contract in the two places it lied ([e326431](https://github.com/dasch-swiss/sipi/commit/e32643198c20d27c5a049b09c5dd0a6183f5a56a))
* **image:** Stop retrying the dispatched handler and say what was tried ([d548659](https://github.com/dasch-swiss/sipi/commit/d54865960b832b6ec2562960156dbf37aeb08066))
* **metadata:** Convert the parsers to Result factories ([a663f8f](https://github.com/dasch-swiss/sipi/commit/a663f8fda491d17f3f6f9ee16d4a74cb7aaaefec))
* **metadata:** Make the ICC chokepoint structurally enforced ([6045492](https://github.com/dasch-swiss/sipi/commit/6045492a1054214a900495151d3c24880d7398ad))


### Documentation

* **adr:** Accept ADR-0007 and bring it to current fact ([a991c55](https://github.com/dasch-swiss/sipi/commit/a991c552102dfc72ab241aad703499adcea99576))
* **adr:** Retire the stale sentry_smoke gate reference in ADR-0015 ([ea64d8d](https://github.com/dasch-swiss/sipi/commit/ea64d8d7e3a63058382e9c064e8e9d8cfc56aa99))
* **docs:** Add IIIF-parser fuzz-harness plan (DEV-6970) ([e6d3818](https://github.com/dasch-swiss/sipi/commit/e6d381800611231599f46cad403fff2aa41d83ee))
* **docs:** Adopt SIPI specs in docs/specs ([abf8edd](https://github.com/dasch-swiss/sipi/commit/abf8edd716104672db1a4a426489262fe7c3d5af))
* **docs:** Adopt value-based errors and record the dissolved-hub topology ([7cd10a8](https://github.com/dasch-swiss/sipi/commit/7cd10a82445c08cd14c6e5d03658da6639066a5b))
* **docs:** Align fuzzing docs with the shipped Rust harness ([ce7342c](https://github.com/dasch-swiss/sipi/commit/ce7342c1da94a07b0adf7662feaa25b9849b0a1f))
* **docs:** Correct the format-handler topology in the architecture docs ([c8a15a2](https://github.com/dasch-swiss/sipi/commit/c8a15a2c9cd595c7e015798164c468b3a8cf30c1))
* **docs:** Describe the image layer's error contract as it now stands ([de6cde9](https://github.com/dasch-swiss/sipi/commit/de6cde9a0dfcef1f8ed592133ee718beca71a81c))
* **docs:** Record the language-subfolder package layout convention ([4c0f8ac](https://github.com/dasch-swiss/sipi/commit/4c0f8ac59c4817c4fb86aa0200ed61957321d575))
* **docs:** Record the value-error types on the error module row ([13febca](https://github.com/dasch-swiss/sipi/commit/13febca5657eac1cd438445746adc755e9a92eb9))
* **docs:** Trim derivable sections from CLAUDE.md ([bd5e891](https://github.com/dasch-swiss/sipi/commit/bd5e891967697d1bc11c512e3f542ee6c8e8c951))
* **format_handlers:** Record the decode-size budget and how to classify a finding ([43a89ff](https://github.com/dasch-swiss/sipi/commit/43a89ff185e18d38c48d8420272c44970cd03eda))
* **formats:** Document the codec fuzz harnesses ([20442b7](https://github.com/dasch-swiss/sipi/commit/20442b750ed9bca95b83a26ccce4ccc335cb7d86))
* **image_processing:** Correct the scale docstrings to match the one-pixel guard ([006d41b](https://github.com/dasch-swiss/sipi/commit/006d41bbda57b3a5ec818695d90ff3fbdc369fd1))
* **image:** Audit raw pixel-buffer and private-member access ([d2edd3c](https://github.com/dasch-swiss/sipi/commit/d2edd3c311cfa25f9c056be15de385c498fc28f9))
* **image:** Record the value-error rule and the exception contract ([264b761](https://github.com/dasch-swiss/sipi/commit/264b7613d354d9fe4d1df8884558c67a632b1adf))
* **specs:** Add codec fuzzing and value-error migration roadmap plan ([f871d20](https://github.com/dasch-swiss/sipi/commit/f871d2023b37bf6452f7f24c5cb7be6b39ad2994))
* **specs:** Add codec memory-safety remediation plan and journal ([fdd45fe](https://github.com/dasch-swiss/sipi/commit/fdd45fe58580905781205280939bfc3da6a9255e))
* **specs:** Add the bazel visibility tightening plan ([463248d](https://github.com/dasch-swiss/sipi/commit/463248d251ea6991d9dec3a6909f952798968f4a))
* **specs:** Close out the coverage and CI acceptance criteria ([4807884](https://github.com/dasch-swiss/sipi/commit/4807884c2a6c3d2d040bff451f9974e940f8974e))
* **specs:** Record the bazel visibility execution in the plan and journal ([3454617](https://github.com/dasch-swiss/sipi/commit/3454617f3bb6a481eb7fda242e442cb2f95b81a9))
* **specs:** Track codec-fuzzing and value-error execution in the plan and journal ([39f5f7e](https://github.com/dasch-swiss/sipi/commit/39f5f7e03335245e4ae79ba3857bdcab6d683c71))


### Tests

* **cache,util,format_handlers:** Co-locate the legacy unit tests with their modules ([6f98f9b](https://github.com/dasch-swiss/sipi/commit/6f98f9bf999d38cc10cae4c44f44bfa230f65cc7))
* **e2e:** Strip ASan log_path for CLI convert/verify subprocesses ([63cca36](https://github.com/dasch-swiss/sipi/commit/63cca36bbd37b60b0e14447d9675cec7a96563ee))
* **format_handlers:** Give the codec fuzz harness a decode-size budget ([e770470](https://github.com/dasch-swiss/sipi/commit/e7704703946ba68b1c6557d27eb35078b4844145))
* **format_handlers:** Pin the fatal-metadata paths with crafted fixtures ([a2ceb0b](https://github.com/dasch-swiss/sipi/commit/a2ceb0b55226e71fde698655f1e3145696638d68))
* **format_handlers:** Register the codec fuzz harnesses with coverage and the strategy doc ([e93c3a9](https://github.com/dasch-swiss/sipi/commit/e93c3a9c09d782ffe5e1b25040ddd28625e6228e))
* **format_handlers:** State what the codec fuzz harness can still catch ([5661f84](https://github.com/dasch-swiss/sipi/commit/5661f848b879f300fa491cf18b2538577cf3f631))
* **formats:** Add libFuzzer harnesses over the codec decode entry points ([05ca1c7](https://github.com/dasch-swiss/sipi/commit/05ca1c7e95c080ee3185764cda28d8b1e183435b))
* **formats:** Add parse_photoshop pointer-overshoot regression fixture ([783210d](https://github.com/dasch-swiss/sipi/commit/783210d10ee4ec2e63cf0b65d27b52d80a23e1f8))
* **formats:** Pin the J2K oversized-dimension rejection with a fixture ([1d284f9](https://github.com/dasch-swiss/sipi/commit/1d284f92d6392c1c122c1427a7044ca8363c672c))
* **iiifparser:** Add Bazel-native libFuzzer harness for parse_request ([a5efb12](https://github.com/dasch-swiss/sipi/commit/a5efb125904214fc31e8be2541d23f6d125bc077))
* **image,formats:** Add deferred JP2 regression fixtures for YCbCr and palette decode ([0ae5f99](https://github.com/dasch-swiss/sipi/commit/0ae5f9925e5c972eaaa86e6548b42c5b98949f6a))


### Build System

* **bazel:** Bump hermetic-llvm to 0.8.18 to unlock macOS libFuzzer ([6518a5e](https://github.com/dasch-swiss/sipi/commit/6518a5e98bffe484e0ebf64a93de3e9b0d91c43f))
* **bazel:** Narrow main-repo visibility to the documented consumer sets ([ffd4c01](https://github.com/dasch-swiss/sipi/commit/ffd4c015d1b2c931060e933fd95ff07956544b13))
* **bazel:** Narrow vendored-dep overlay visibility to actual consumers ([33e53f2](https://github.com/dasch-swiss/sipi/commit/33e53f281c6ef747b5f3e3459a7421da27809b56))
* **deps:** Exclude Kakadu's palette-entry arithmetic from UBSan ([ab6c378](https://github.com/dasch-swiss/sipi/commit/ab6c378dd3ccc45854d9adb95f37ee0b3902633b))
* **format_handlers:** Declare the direct value-objects dependency ([b9d7bc2](https://github.com/dasch-swiss/sipi/commit/b9d7bc287961a9eb7c322ab13526389258ef3578))
* **formats:** Wire the codec fuzz harnesses into the fuzzing loop ([3c6a834](https://github.com/dasch-swiss/sipi/commit/3c6a834d01d3a3456c56b4f7bbf3b6d9b8745c26))


### Miscellaneous Chores

* **ci:** Add nightly libFuzzer workflow for the IIIF parser ([5d9227d](https://github.com/dasch-swiss/sipi/commit/5d9227d6db7e05734a7fbc533343197145770e65))
* **claude:** Drop sentry plugin from enabled plugins ([37363ef](https://github.com/dasch-swiss/sipi/commit/37363eff706db5660c24482dab41d3ee70a4bd1d))

## [7.0.0](https://github.com/dasch-swiss/sipi/compare/v6.4.1...v7.0.0) (2026-08-21)


### ⚠ BREAKING CHANGES

* **scripting:** script-facing divergences from the previous runtime (each documented in ADR-0023 and docs/src/lua): config.password, config.adminuser, server.shutdown, and server.fs.chdir are removed; the stdlib is whitelisted (io/debug absent, os reduced to getenv/clock/date); require is restricted to the script dir; decode_jwt pins HS256 and requires a valid exp claim; server.http no longer follows redirects, caps response bodies at 16 MiB, and treats the timeout as total-request; server.cookies returns one entry per cookie with original-case names; config size strings parse strictly; scripts run under a memory cap and deadline (SIPI_LUA_MEMORY_LIMIT, SIPI_LUA_TIMEOUT_MS).

### Features

* **observability:** Export sipi.lua.kills{reason} from the runtime kill stats ([e44f5f5](https://github.com/dasch-swiss/sipi/commit/e44f5f5e5ce2a7993b472ce31146ff67120827f2))
* **observability:** Per-entry-point Lua VM-build and script-duration histograms ([7fa0188](https://github.com/dasch-swiss/sipi/commit/7fa01880c3437f58627f2f1d25cae85b19c5ba60))


### Bug Fixes

* **logging:** Correct the CRIT/ALERT/EMERG level labels ([d0ed51f](https://github.com/dasch-swiss/sipi/commit/d0ed51f2703d5560e8e16a5b44df9e75c8898901))
* **lua:** Call the send_error global, not the never-registered server.send_error ([ed1b2b5](https://github.com/dasch-swiss/sipi/commit/ed1b2b5ee5c7a166d6ff9d4e6b056ba027e84179))
* **throttling:** Admit Lua routes and docroot scripts through the Full lane ([88b1239](https://github.com/dasch-swiss/sipi/commit/88b1239291d21022995e77814aeba4843f5925a8))


### Code Refactoring

* **ffi:** Add the sipi_image_* handle ABI for the Lua runtime ([3d6790c](https://github.com/dasch-swiss/sipi/commit/3d6790c6634397a5ed5b31d3c2772d35e60d4408))
* **ffi:** Carry hostname/sslport over the seam and redact secrets in Debug ([f98ce35](https://github.com/dasch-swiss/sipi/commit/f98ce354401ba74f6cc740304e59e18ca0e1525f))
* **scripting:** Add the mlua runtime core — VM profile, limits, bytecode cache ([90a38f6](https://github.com/dasch-swiss/sipi/commit/90a38f6a35829203fc5061ce6165ebc80ee7bf49))
* **scripting:** Delete the C++ Lua runtime ([2042b1e](https://github.com/dasch-swiss/sipi/commit/2042b1efe1ec6a0732f8be9e7052f91be51f586e))
* **scripting:** Parse the Lua config in Rust and delete the C++ config parse ([e6b042e](https://github.com/dasch-swiss/sipi/commit/e6b042eb1792e75c1f3e82858a0b9c16da98ce0e))
* **scripting:** Pure-Rust server.* / config / helper bindings ([e4bc618](https://github.com/dasch-swiss/sipi/commit/e4bc618dadbbcc18456aa927e40c0672e435a49d))
* **scripting:** Serve Lua routes and docroot scripts from the Rust runtime ([971ce5d](https://github.com/dasch-swiss/sipi/commit/971ce5ded7ecd60e352534708b5ec226c87215cc))
* **scripting:** Serve preflight from the Rust runtime, delete the seam entries ([d484644](https://github.com/dasch-swiss/sipi/commit/d4846446ac946b53a67312f7cceece2dd3ac7fa6))
* **scripting:** SipiImage, sqlite, and helper bindings over the engine ABI ([bbb8244](https://github.com/dasch-swiss/sipi/commit/bbb8244bad4db71f6c208dc530c9668bc73ce014))
* **server-rs:** Carry Result&lt;Bytes, BodyAbort&gt; on the body channel ([806f425](https://github.com/dasch-swiss/sipi/commit/806f425b121ac0e368ad65124c66bccb7e8d327d))


### Documentation

* **build:** Add the src/ module-dependency diagram ([0969bcc](https://github.com/dasch-swiss/sipi/commit/0969bccbc3cb36d11eba2ff42befbf852c485d8c))
* **ci:** State the phase-1 model evidence precisely ([9fb10f2](https://github.com/dasch-swiss/sipi/commit/9fb10f266c73dbcc6b6072b003b6e5529f3d5d9c))
* **scripting:** Add ADR-0023 for the Rust-hosted mlua Lua runtime ([68d8bc4](https://github.com/dasch-swiss/sipi/commit/68d8bc4ec80b12aed074d99afc3a191e63977a72))
* **scripting:** Add the request-flow overview diagram ([4d54aa5](https://github.com/dasch-swiss/sipi/commit/4d54aa57770fdbe67f4666eeb31a0c8778ab887a))
* **throttling:** Add the threads-and-permits stage diagram ([8c6d54a](https://github.com/dasch-swiss/sipi/commit/8c6d54abcd4d3f1676ded0acd82925c7746cd15b))


### Tests

* **scripting:** Hardening and dsp-api-closure e2e suites ([818c874](https://github.com/dasch-swiss/sipi/commit/818c8746b3fa35d4a818947b512502ff2b9269ed))
* **scripting:** Paired C++/Rust per-request Lua VM microbenchmarks ([510c589](https://github.com/dasch-swiss/sipi/commit/510c589cf9af8fd0877ec4f35e56e712c2a891f6))


### Build System

* **deps:** Enable mlua (lua53 + external) against the BCR [@lua](https://github.com/lua) ([78f65ca](https://github.com/dasch-swiss/sipi/commit/78f65ca2a3fbe7ee04b3a968cf406a62eef66684))


### Miscellaneous Chores

* **ci:** Gate the review on a verified wrong outcome, and pin Opus 5 at medium effort ([b77ec60](https://github.com/dasch-swiss/sipi/commit/b77ec60992f487289aef784a27bfa841788216cb))
* **ci:** Split the review out of the shared Claude job and align its tool set ([45a4e4f](https://github.com/dasch-swiss/sipi/commit/45a4e4f25383007bbd87e2100e36afa15d2c0b93))
* **logging:** Drop commented-out debug prints and their dead helper ([c32be23](https://github.com/dasch-swiss/sipi/commit/c32be23447d748f28cbf287f3443b4121d554f46))

## [6.4.1](https://github.com/dasch-swiss/sipi/compare/v6.4.0...v6.4.1) (2026-08-19)


### Bug Fixes

* **server-rs:** Derive info.json scaleFactors/sizes from one tile-grid pyramid ([ac320ba](https://github.com/dasch-swiss/sipi/commit/ac320bad72b132ebd73273929b65e1d3b83690f2))


### Code Refactoring

* **ffi:** Drop the dead clevels field from SipiImageDims ([9016a1d](https://github.com/dasch-swiss/sipi/commit/9016a1d9154752c32f2f6244a03641f7a9a1e9e4))

## [6.4.0](https://github.com/dasch-swiss/sipi/compare/v6.3.1...v6.4.0) (2026-08-18)


### Features

* **observability:** Expose runtime admission metrics over OTLP ([1878570](https://github.com/dasch-swiss/sipi/commit/187857094ffe32adc39d518f335e99bdc68f030c))
* **throttling:** Full-lane memory budget + admission mode over the seam ([542b3b2](https://github.com/dasch-swiss/sipi/commit/542b3b2b82efce968db0dac6b8b93204c46e3978))
* **throttling:** Two-lane admission pool (admission crate) + shell integration ([51a0126](https://github.com/dasch-swiss/sipi/commit/51a01266ce7e7c08952ef9c7abd97e98b63e1a13))


### Bug Fixes

* **ci:** Gate Claude workflow triggers to org members (DEV-6884) ([8cfcf25](https://github.com/dasch-swiss/sipi/commit/8cfcf25d6926a6be4f82e8f9adf4d9dfbd4e8a3b))
* **observability:** Remap allocator arena/retained gauges to true RSS ([210c179](https://github.com/dasch-swiss/sipi/commit/210c1791af1de22d5fd035911ec14880d3590ae9))


### Code Refactoring

* **bazel:** Carve iiifparser and formats into their own packages ([171c343](https://github.com/dasch-swiss/sipi/commit/171c343b64b593eca4788ec043662db5b02425c1))
* **ffi:** Collapse the served-image format↔mime pairing to one table ([223fbd1](https://github.com/dasch-swiss/sipi/commit/223fbd15b3bd809c6b64f8fc9b7708a19ed9e1b5))
* **ffi:** Move sipi_init out of the oracle label into the seam ([236d22c](https://github.com/dasch-swiss/sipi/commit/236d22c7062debac0d7b787e82cc6f0505c3fed9))
* **ffi:** Remove the dead config surface and apply the configured engine log level ([b1fdeb1](https://github.com/dasch-swiss/sipi/commit/b1fdeb13ffbf89fbcac48c48524e7e847f41c5df))
* **handlers:** Remove the per-IP rate limiter ([1ec8492](https://github.com/dasch-swiss/sipi/commit/1ec84925c44045f547ab1c569edf68d124f1f013))
* **iiifparser:** Colocate the polyglot implementations and carve a domain-typed Rust parser ([a0c56d1](https://github.com/dasch-swiss/sipi/commit/a0c56d1f872f07fe1893748d7976d9ad1c358daf))
* **iiifparser:** Fold the parse_iiif_uri classifier out of src/handlers ([bc0e09c](https://github.com/dasch-swiss/sipi/commit/bc0e09cc086dfc166de9cd5a6b7294416f7bdcb5))
* **server-rs,cli-rs:** Make the config seam fail on a dropped field ([6e5eeb3](https://github.com/dasch-swiss/sipi/commit/6e5eeb3fc75e87a9a4bf85e388a6c9bcdf70b188))
* **server-rs,cli-rs:** Sweep oracle-era framing from Rust comments ([78c1cee](https://github.com/dasch-swiss/sipi/commit/78c1cee9d82bb33647c6ba15a6139ed9219a59e9))
* **server-rs,ffi,iiifparser:** Align code with the ubiquitous language ([1e3ecc6](https://github.com/dasch-swiss/sipi/commit/1e3ecc681d977c97714619d4e9094b3802ddcdd4))
* **shttps:** Decompose into scripting/util/jwt, out of src/shttps ([fa66a6a](https://github.com/dasch-swiss/sipi/commit/fa66a6a25286775065802f47b86a150412988e44))
* **shttps:** Remove the C++ oracle server ([b7e34b3](https://github.com/dasch-swiss/sipi/commit/b7e34b3f825153444eb5e345ba3af5e2509fc085))
* **throttling:** Carve the memory budget into src/throttling/cpp ([fb7030a](https://github.com/dasch-swiss/sipi/commit/fb7030a0eb1b73ea11962fbdd5a58788d0fb0aad))
* **throttling:** Rename admission_mode to basic/advanced, take pool config off the Lua surface ([937ef86](https://github.com/dasch-swiss/sipi/commit/937ef86b4891c080c4b3384f911f1263b11b4061))


### Documentation

* **cli:** Drop the stale "differential-test oracle" label from sipi.cpp ([53ff8a9](https://github.com/dasch-swiss/sipi/commit/53ff8a9eb388cf7b05d0c82d6433c5e42c6db6d9))
* **docs:** Bootstrap ARCH-MAP.md from the oracle-free tree (DUNE-001) ([f720d00](https://github.com/dasch-swiss/sipi/commit/f720d00eb77daaf6ad24440d8bc184162f483149))
* **docs:** Correct the operator pool-knob and port docs against the code ([b5557e2](https://github.com/dasch-swiss/sipi/commit/b5557e23c9775d9cb435aed70b76515e509eaa67))
* **docs:** Reconcile agent-context docs with the current tree ([d4e326d](https://github.com/dasch-swiss/sipi/commit/d4e326d2807b4b742bb463504c18b6062b4e2ec1))
* **docs:** Scrub dangling deleted-oracle-file citations the .rs gate can't reach ([d528dab](https://github.com/dasch-swiss/sipi/commit/d528dabcd78eafee452338b283ee4f220e3f8a47))
* **formats:** Add in-place invariant banners to the codec + metrics surfaces ([003614b](https://github.com/dasch-swiss/sipi/commit/003614b99656fb7b6d9e77424fe308926fb8241e))
* **handlers:** Remove the rate-limiter documentation ([e906d5c](https://github.com/dasch-swiss/sipi/commit/e906d5cc652daec8c4073ceeab6a88b3651bb639))
* **iiifparser:** Add ADR-0021 for polyglot colocation and domain-typed Rust parser ([5398d56](https://github.com/dasch-swiss/sipi/commit/5398d569be5fd00bb184fb790b8ba4b878212bb3))
* Memory-budget.md, running.md, and the sample Lua config updated. ([542b3b2](https://github.com/dasch-swiss/sipi/commit/542b3b2b82efce968db0dac6b8b93204c46e3978))
* **observability:** Document per-partition admission + 413 metrics and the RSS remap ([1302ec7](https://github.com/dasch-swiss/sipi/commit/1302ec7e55ffe86c3b23ff238d3040de7348ba01))


### Tests

* **ffi:** Guard every seam struct and enum against silent drift ([80d9327](https://github.com/dasch-swiss/sipi/commit/80d9327a65ff1c366285f0b1dc55d71af5a58db8))
* **iiifparser,formats,logging:** Colocate carved-module unit tests (ADR-0003) ([2654ea5](https://github.com/dasch-swiss/sipi/commit/2654ea5c84cf5cb1c8b7f41626fcfc81c844b214))
* **throttling:** E2e admission suite for enforce-mode 413 + rewrite stale memory_budget ([5d396eb](https://github.com/dasch-swiss/sipi/commit/5d396ebbbe1525f7ffd39fe9dcabb887b29009f6))


### Miscellaneous Chores

* **bazel:** Drop the stale observability→shttps grant, fix layering rationale ([6d6b2c7](https://github.com/dasch-swiss/sipi/commit/6d6b2c79d476f55b264bab913e11b28f3e2de643))
* **claude:** Drop unresolved my-eng plugin from enabled plugins ([032b90b](https://github.com/dasch-swiss/sipi/commit/032b90bd4edd22bf2d6bb68bdb9eb96b87aa33e8))

## [6.3.1](https://github.com/dasch-swiss/sipi/compare/v6.3.0...v6.3.1) (2026-08-04)


### Bug Fixes

* **memory:** Read mimalloc stats through a header-checked C shim ([aa52ec2](https://github.com/dasch-swiss/sipi/commit/aa52ec22b338e26af665e84b9c1bf27769dab6e3))


### Miscellaneous Chores

* **bazel:** Fan single-host distfile fetches out to verified mirrors ([e27d51d](https://github.com/dasch-swiss/sipi/commit/e27d51d5d115cc68601692768a237774e035f890))
* **ci:** Save the Bazel repo cache even when a job fails ([4a48668](https://github.com/dasch-swiss/sipi/commit/4a48668424c30cafeea4528bd675b8be7ff72b91))

## [6.3.0](https://github.com/dasch-swiss/sipi/compare/v6.2.3...v6.3.0) (2026-07-29)


### Features

* **observability:** Export process-allocator gauges over OTLP ([329b09c](https://github.com/dasch-swiss/sipi/commit/329b09c2ba5d17a573ddd208b258701169217224))


### Performance Improvements

* **memory:** Switch the production allocator to mimalloc v3 ([923adaa](https://github.com/dasch-swiss/sipi/commit/923adaad4173dc481f90e4bdd428ed613743485e))


### Tests

* **memory:** Add the allocator replay harness ([da2b044](https://github.com/dasch-swiss/sipi/commit/da2b0441ef432c9cdb983922800e4ff64281061a))

## [6.2.3](https://github.com/dasch-swiss/sipi/compare/v6.2.2...v6.2.3) (2026-07-28)


### Performance Improvements

* **docker:** Cap glibc malloc arenas in the container image ([444518d](https://github.com/dasch-swiss/sipi/commit/444518db1cf8accd01cd8204752a4943e517d410))


### Miscellaneous Chores

* **bazel:** Pin LC_ALL for macOS test actions to avoid nix-bash locale segfault ([a2393d7](https://github.com/dasch-swiss/sipi/commit/a2393d7fd2703011c664ba3ec34543413daf2542))

## [6.2.2](https://github.com/dasch-swiss/sipi/compare/v6.2.1...v6.2.2) (2026-07-26)


### Bug Fixes

* **formats:** Decode TIFF from the matching pyramid level ([f38b0c0](https://github.com/dasch-swiss/sipi/commit/f38b0c0151ae50ba12fad5591a43fb4bd38825ed))
* **observability:** Carry the decode-memory estimate across the serve seam ([870d598](https://github.com/dasch-swiss/sipi/commit/870d598e52f158c0e81546b8296820ee26438cd4))
* **observability:** Record http.server.request.duration on the serve path ([42cbe42](https://github.com/dasch-swiss/sipi/commit/42cbe4240d2965a69bd7c468ab8904e09582db80))
* **observability:** Stamp service.version from the engine build version ([3a817bb](https://github.com/dasch-swiss/sipi/commit/3a817bb608a765df95f0a2c582e2ae9cba996183))


### Performance Improvements

* **image:** Resample scale() in fixed-point integer arithmetic ([861bafe](https://github.com/dasch-swiss/sipi/commit/861bafec8e546ac539f42253e8da1c6bbb0dd3a3))
* **image:** Resample scale() with a separable two-pass filter ([567cc30](https://github.com/dasch-swiss/sipi/commit/567cc30b3537b844e0ab79912c866395dee1b632))
* **image:** Vectorize the resampler with Highway SIMD ([ff27cfa](https://github.com/dasch-swiss/sipi/commit/ff27cfabf97bc34f1b0abe2bfad961ad1da1c321))


### Documentation

* **commit-conventions:** Require a scope, drop revert/style/ci, single-source the vocabulary ([a11a3c2](https://github.com/dasch-swiss/sipi/commit/a11a3c285f0d2738967cf660fcecdfffee0765cb))
* Scope commits by concern, not code location ([e4f14af](https://github.com/dasch-swiss/sipi/commit/e4f14af79d47e80491cfe384389713b717ecdbe8))


### Tests

* **e2e:** Split the cli target so JP2 conversions run in parallel ([4d916cd](https://github.com/dasch-swiss/sipi/commit/4d916cd458c45c8596db3964f913bd36921dc796))
* **iiifparser:** Cover reduce-driven crop_coords and size reduce levels ([82c9f26](https://github.com/dasch-swiss/sipi/commit/82c9f260c534a06baba3e696bb6c85d921c3f3cc))
* **iiifparser:** Lock percent-reduce selection and crop_coords level consistency ([314e5ad](https://github.com/dasch-swiss/sipi/commit/314e5ade231a63c1b7ebdd7e4eba46dc6160ca86))
* **image:** Characterize scale() resampler output ([2428e34](https://github.com/dasch-swiss/sipi/commit/2428e34a6bb38bd609b94a27e2ac4cc47367be55))
* **image:** Shard sipi_image_tests and drop a redundant pyramid write ([62fd2bb](https://github.com/dasch-swiss/sipi/commit/62fd2bb531cc44d3b9f3f7e70e9b888a6980d80b))
* **observability:** Assert cache growth on disk instead of the removed /metrics route ([17b3d18](https://github.com/dasch-swiss/sipi/commit/17b3d18306b35f7222d976995b78dc6170395ee2))
* **observability:** Trip on metrics that stop at the FFI seam ([2509c98](https://github.com/dasch-swiss/sipi/commit/2509c98d0ac9eb643211534f5d748725c9c0a22a))


### Miscellaneous Chores

* **ci:** Enforce commit type and scope with commitlint-rs ([854a14c](https://github.com/dasch-swiss/sipi/commit/854a14c86291960eda4d604fd46df211ad749ded))
* **ci:** Merge the sanitizer jobs and run e2e tests on RBE ([11d4994](https://github.com/dasch-swiss/sipi/commit/11d4994c7861e0244848fee407918b4e33eba6d1))
* **ci:** Run the differential parity gate on the RBE worker ([9885b00](https://github.com/dasch-swiss/sipi/commit/9885b00c24b4dde82c6d81d5179caf52d168082b))

## [6.2.1](https://github.com/dasch-swiss/sipi/compare/v6.2.0...v6.2.1) (2026-07-25)


### Bug Fixes

* **formats:** Close TIFF handles and buffers via RAII on all read/write paths ([82597ed](https://github.com/dasch-swiss/sipi/commit/82597edde3f6fda5d9367006ccd0711ab8a80dc4))
* **formats:** Make PNG text chunks and FILE handles RAII-safe ([0832860](https://github.com/dasch-swiss/sipi/commit/08328600a2ce0c7ea6e9ffa199d067fa3860ae69))
* **formats:** Move JPEG source/destination ownership out of the longjmp window ([1c1d0c3](https://github.com/dasch-swiss/sipi/commit/1c1d0c3511f99d9a34704730934c16caf6bf798a))
* **formats:** Tear down Kakadu decode machinery exactly once via RAII ([68e4b01](https://github.com/dasch-swiss/sipi/commit/68e4b01dbc00e6dfbac867a11bbe2a88bff73284))
* **image:** Compare 16 bps pixels against rhs in operator== ([15aaf5d](https://github.com/dasch-swiss/sipi/commit/15aaf5d13d4a049873c7b26ecb7c9bebe7b0cbd9))
* **metadata:** Give Exif and Icc correct ownership semantics ([74737bd](https://github.com/dasch-swiss/sipi/commit/74737bdc64bb385cc5d9dc13c8147f3f5e2e7808))
* **shttps:** Stop resource leaks on Lua, JWT, curl, and JSON error paths ([3c76649](https://github.com/dasch-swiss/sipi/commit/3c76649c7b827cee548ad6e1c6679dbd49f821be))


### Performance Improvements

* **formats:** Multithread JP2 decode via kdu_thread_env ([7bdb634](https://github.com/dasch-swiss/sipi/commit/7bdb6349562f4fb3f737bb74ba2349b8ece8b1c3))


### Documentation

* Correct the infra repo name (ops-infra → infra) ([865bc41](https://github.com/dasch-swiss/sipi/commit/865bc415f36d38137d8ba1c0ca1248a64434a186))


### Miscellaneous Chores

* Enable misc plugin ([472974f](https://github.com/dasch-swiss/sipi/commit/472974f72e81cdcaf7f12c7413e31ddd688f8e16))


### Code Refactoring

* **image:** Own the pixel buffer as std::vector ([955c450](https://github.com/dasch-swiss/sipi/commit/955c4506ae9e717858b36c00cae7b4c79301425f))
* **shttps:** Finish the RAII cleanup sweep ([bf2f391](https://github.com/dasch-swiss/sipi/commit/bf2f391ce8252ea2eb9bed67ce6cb4d091fc25b4))


### Tests

* Add concurrent-load decode harness ([f5daa39](https://github.com/dasch-swiss/sipi/commit/f5daa39a272357aa0d25a01f549a35c3b55c5332))
* **image:** Cover the IIIF transform pipeline at the unit sanitizer layer ([dad9026](https://github.com/dasch-swiss/sipi/commit/dad902643b076b44b3b4d7a1764e1d80508d3f40))


### Build System

* **bazel:** Cut RBE write pressure from the test path ([5d3f9fe](https://github.com/dasch-swiss/sipi/commit/5d3f9fe37b38252d0f91cd2541c989783b2a563c))


### Continuous Integration

* Attach Bazel build-event JSON for cache-hit-rate analysis ([c01f6af](https://github.com/dasch-swiss/sipi/commit/c01f6afa4100823ee4b6f993c2bb7953c2a61ace))
* Surface every commit type in the release notes ([f63fd0e](https://github.com/dasch-swiss/sipi/commit/f63fd0eb698433fe464549826af2845177987ad6))

## [6.2.0](https://github.com/dasch-swiss/sipi/compare/v6.1.0...v6.2.0) (2026-07-23)


### Features

* **server-rs,cli-rs:** Cache pre_flight access decisions per (image, credential) ([61fd954](https://github.com/dasch-swiss/sipi/commit/61fd9544ca6928379871526c458e710985f25168))
* **server-rs,ffi:** Trace engine serve phases as child spans ([d650e26](https://github.com/dasch-swiss/sipi/commit/d650e26c4f22b009b3679ac8f45420157e78214b))

## [6.1.0](https://github.com/dasch-swiss/sipi/compare/v6.0.0...v6.1.0) (2026-07-23)


### Features

* **server-rs,cli-rs:** Queue requests at the engine pool instead of shedding ([6cc6f2b](https://github.com/dasch-swiss/sipi/commit/6cc6f2b9c6998d9935b5d1d2a4a9597a21c981ba))

## [6.0.0](https://github.com/dasch-swiss/sipi/compare/v5.0.1...v6.0.0) (2026-07-22)


### Features

* **cli-rs:** Parse the full `server` flag surface (unwired) ([29b43c2](https://github.com/dasch-swiss/sipi/commit/29b43c20aaac441461e5f9705a27923756d66b15))
* **ffi:** Apply CLI/env overrides onto SipiConf in sipi_init ([20f958b](https://github.com/dasch-swiss/sipi/commit/20f958baee213699218e10fed37f5a1326d1febb))
* **ffi:** Define the concrete SipiServerConfig override struct ([e749716](https://github.com/dasch-swiss/sipi/commit/e749716b4e6638a74dcc8d95bf91182c4ab6fced))
* **ffi:** Emit the IIIF profile Link header on image responses ([c7d23ca](https://github.com/dasch-swiss/sipi/commit/c7d23cafb97cd5589f3a4e163d130a7d4492a587))
* Forward the full server CLI/env flag set into ServerOverrides ([2f5dbb1](https://github.com/dasch-swiss/sipi/commit/2f5dbb1bdfbfd1de2dcaf6c26b289376085ee555))
* Propagate W3C traceparent from Lua outbound calls to dsp-api ([0f7851e](https://github.com/dasch-swiss/sipi/commit/0f7851e194fa635b63933efa4d28ecdc514810b5))
* Serve the /server docroot fileserver in the Rust shell ([221c94a](https://github.com/dasch-swiss/sipi/commit/221c94a0a68532445767d28c9f2dc52d83407014))
* **server-rs,cli-rs:** Cut over crash reporting from sentry-native to Rust sentry + minidump ([a44ba04](https://github.com/dasch-swiss/sipi/commit/a44ba047e0efcc93599892a8b7ea41d0335ed931))
* **server-rs:** Add a sipi_port FFI getter as the listener fallback ([06f4cb8](https://github.com/dasch-swiss/sipi/commit/06f4cb8daf31fd6cca15de28b78ada7a435c1175))
* **server-rs:** Bridge engine + pool metrics to OTLP observable instruments ([501e439](https://github.com/dasch-swiss/sipi/commit/501e439c5132f6f3647678c7328b2b945d378eec))
* **server-rs:** Forward overrides through sipi_init via OverridesHolder ([2a68fee](https://github.com/dasch-swiss/sipi/commit/2a68fee2e2e00a444510242c3407fc01017d8351))
* **server-rs:** Mirror SipiServerConfig as repr(C) + layout test ([a2126d5](https://github.com/dasch-swiss/sipi/commit/a2126d5d47ed43635a58cd68d84966a0ac3fcc72))
* Support TOML config files for the server (--config *.toml) ([b36d374](https://github.com/dasch-swiss/sipi/commit/b36d374d741d2f02dd98ba90070299f6f5cfb20b))


### Bug Fixes

* **ci:** Drop the remote downloader, cache external deps via repository_cache ([95274dc](https://github.com/dasch-swiss/sipi/commit/95274dce84e5d1b011d614e6da01072df6535d39))
* **ci:** Repair lint job's bazel-rbe inputs so lint actually uses RBE ([dc3035b](https://github.com/dasch-swiss/sipi/commit/dc3035b9867d1050be1d78accfc509a7004f859e))
* **cli-rs:** Exit 0 on `server --help` / `--version` ([ad25ddd](https://github.com/dasch-swiss/sipi/commit/ad25ddddbdd1b3c293a1a890eaa6739c666ccd98))
* **cli-rs:** Replace ureq with a raw TCP probe in the health verb ([325ae95](https://github.com/dasch-swiss/sipi/commit/325ae95d2bebd0b7be4541a018d8c93e8a357196))
* **cli:** Guard the empty --maxpost length underflow ([4db9285](https://github.com/dasch-swiss/sipi/commit/4db928545382e720fbd72d271cad68a44391eb1e))
* **cli:** Reject negative cache-nfiles + range-check ports (both binaries) ([a39298d](https://github.com/dasch-swiss/sipi/commit/a39298d0e8e2c29bd86a1e260cdb44626e858099))
* **cli:** Sipi compare per-channel delta uses absolute difference ([f7e397a](https://github.com/dasch-swiss/sipi/commit/f7e397a553531a9f3817f7fcf0b7be36e34ffc69))
* **cli:** Stringify --quality before storing it in compression params ([5cc59e9](https://github.com/dasch-swiss/sipi/commit/5cc59e95a1f79a6875b9e6b102a6e5d83901ca67))
* **e2e:** Stop racing subject and reference on a shared cache dir ([563fbd2](https://github.com/dasch-swiss/sipi/commit/563fbd2de56dd1c6ad980676b3c1220a57096c81))
* **ffi:** Clamp negative max-decode-memory/maxpost overrides to 0 ([ce25eb8](https://github.com/dasch-swiss/sipi/commit/ce25eb8c0308c7e4a33a10c2b39dcf473273d446))
* **ffi:** Return 404 when a Lua-route script vanishes after the access check ([74dbf48](https://github.com/dasch-swiss/sipi/commit/74dbf48d32262637acdd9fcc6087a1445cd27441))
* **formats:** Convert corrupt-JP2 Kakadu errors to SipiImageError ([902519d](https://github.com/dasch-swiss/sipi/commit/902519da63b7b62054462acf9cb80bd9a888d0d2))
* **formats:** Make SipiImage::write reject unknown format keys ([61c9eb5](https://github.com/dasch-swiss/sipi/commit/61c9eb542cbf3f839f27d8f839abdb3137fd930a))
* **formats:** Single-thread Kakadu JP2 encode under ASan ([a8b1c38](https://github.com/dasch-swiss/sipi/commit/a8b1c386bea3fae74feaa20833d6f94a218ad522))
* **image:** GetPixel/setPixel use row-major pixel indexing ([8ef46f2](https://github.com/dasch-swiss/sipi/commit/8ef46f2aeccbe615a78a9bd02362b3288373d731))
* **jpeg:** Deterministic cross-architecture JPEG (integer decode + baseline encode) ([5ba1a05](https://github.com/dasch-swiss/sipi/commit/5ba1a0596972bbd08eefa8cfb51b846e6d21520e))
* **just:** Bench-compare drops empty FLAGS arg in no-flags invocation ([fe99532](https://github.com/dasch-swiss/sipi/commit/fe995327e9561d3ef43ca456136a75075ee9ec91))
* **server-rs:** Address adversarial-review findings on the port precedence work ([b200216](https://github.com/dasch-swiss/sipi/commit/b2002167963e3ff43d6cfaa6c6d4f73fee19843a))
* **server-rs:** Exit via _exit() under ASan to skip teardown-time join abort ([e484f6a](https://github.com/dasch-swiss/sipi/commit/e484f6a3c6ca9291f53b643249689da1a60243b0))
* **server-rs:** Keep tokio's blocking pool alive under ASan ([137265c](https://github.com/dasch-swiss/sipi/commit/137265c27091423dbb3a7b279fe3f9744ffc03da))


### Miscellaneous Chores

* Release 6.0.0 ([51b7cb7](https://github.com/dasch-swiss/sipi/commit/51b7cb74a74f9f6e854ea0f7e9415ccca4c30301))

## [5.0.1](https://github.com/dasch-swiss/sipi/compare/v5.0.0...v5.0.1) (2026-06-03)


### Bug Fixes

* **cli:** Shut down sentry transport before curl cleanup on all exit paths ([cfcb38b](https://github.com/dasch-swiss/sipi/commit/cfcb38b1e976867c36a7a8a92969e7189b57957c))

## [5.0.0](https://github.com/dasch-swiss/sipi/compare/v4.1.1...v5.0.0) (2026-05-28)


### ⚠ BREAKING CHANGES

* **cli:** bare `sipi --convert ...` (and --query, --compare) now fails with a CLI11 usage error. Operators must use the verb-noun subcommand surface introduced in the previous commit:
* **lua:** The HTTP endpoints `GET /api/cache` and `DELETE /api/cache` are removed. External cache management requires either a future dedicated C++ route in route_handlers/ or scraping Prometheus metrics for inspection. The architectural principle established here (Lua = request-shaping; mutation = C++ route) will guide subsequent script-removal decisions in Probe 8 (e.g. exit.lua).

### Features

* Add --version flag to CLI ([0c5da66](https://github.com/dasch-swiss/sipi/commit/0c5da66130e7cecf3769aab57d789cc3371a8d7f))
* **cli/server:** Intentional Service File creation ([980aa8a](https://github.com/dasch-swiss/sipi/commit/980aa8af1bea057158414555234a9125f9018a1e))
* **cli:** Add `sipi health` subcommand for container healthchecks ([6dd5dc6](https://github.com/dasch-swiss/sipi/commit/6dd5dc671fd955a5519107d877e6b015d89eaa50))
* **cli:** Remove legacy --convert / --query / --compare flag forms ([7179451](https://github.com/dasch-swiss/sipi/commit/7179451baf8317baa29e1cc8a1bc2d23f6eace95))
* **format:** Expand EXIF metadata coverage and harden rational-array path ([2f6970f](https://github.com/dasch-swiss/sipi/commit/2f6970f35cec9016e5c957346b8e3c88bbfa4685))
* **formats:** Read_shape fast path + cache shrinkage ([04b9dc3](https://github.com/dasch-swiss/sipi/commit/04b9dc33b06878d90d8e528a275e766e4dad6435))
* **formats:** Service File Essentials carriers (JP2 + pyramidal TIFF) ([4017a03](https://github.com/dasch-swiss/sipi/commit/4017a03f8107b8a890dde677b6622ae68258faf2))
* **icc:** Normalize ICC profile creation date when SOURCE_DATE_EPOCH is set ([45f1caf](https://github.com/dasch-swiss/sipi/commit/45f1caf21d47cc29f45d0065695575f449a300e5))
* **lua:** Remove cache.* Lua bindings and /api/cache admin endpoint ([e412b25](https://github.com/dasch-swiss/sipi/commit/e412b254a8e8048411c6c8302d0aaa3a9ea396f0))
* **metadata:** Protobuf Essentials wire format API ([e130ced](https://github.com/dasch-swiss/sipi/commit/e130ceda8a6c02e3ea67ed1af4a06c5a72262ca5))
* **observability:** Read_shape fast-path + Essentials hash-mismatch metrics ([a8efe11](https://github.com/dasch-swiss/sipi/commit/a8efe119af20c668d3a00fee16bac427871e1778))


### Bug Fixes

* **exif:** Plug leak when EXIF parser throws on malformed bytes ([79ddfe3](https://github.com/dasch-swiss/sipi/commit/79ddfe33443e9295182b88b898da937bbef8cf11))
* Handle --version before library init to avoid LSan exit-time leaks ([269e8fe](https://github.com/dasch-swiss/sipi/commit/269e8fe86d6d7f694dacd3ba96fa0ab97d71d197))
* **http:** Treat any OUTPUT_WRITE_FAIL as client abort ([311578f](https://github.com/dasch-swiss/sipi/commit/311578f780b5bfde39b4cd11f3219395ea78ea0a))
* **icc:** Bail on gmtime_r failure / out-of-range year ([408a018](https://github.com/dasch-swiss/sipi/commit/408a0181d1b2d7b2bb2bcb6ececcdbe34ca2b999))
* **sanitizer:** Symbolize stack frames so Lua leak suppressions match ([fc96101](https://github.com/dasch-swiss/sipi/commit/fc961012b79f37a9be6a669aa629715a21c4149e))


### Reverts

* Remove mkdocs from nix devShells ([99e72be](https://github.com/dasch-swiss/sipi/commit/99e72be2bce7c9a19b92a68e19675677002b604c))

## [4.1.1](https://github.com/dasch-swiss/sipi/compare/v4.1.0...v4.1.1) (2026-04-27)


### Bug Fixes

* Create GitHub Release if missing in publish-static-release ([3a58fa8](https://github.com/dasch-swiss/sipi/commit/3a58fa83a91df41da3d18928e65a3dcd044ae20f))
* **http:** Distinguish client-aborted HTTP writes from real write errors ([53c9d72](https://github.com/dasch-swiss/sipi/commit/53c9d72d9bd58ef8a86e47590e92ef1a3d9eee13))
* **iiif:** Replace std::regex with hand-rolled validators ([526890e](https://github.com/dasch-swiss/sipi/commit/526890e562ca2a7f49697aef330b243b48126f3d))
* **shttps:** Urldecode no longer infinite-loops on trailing % ([1659539](https://github.com/dasch-swiss/sipi/commit/16595399a1c09e91290f5e4d3d854cd267ba84cf))

## [4.1.0](https://github.com/dasch-swiss/sipi/compare/v4.0.1...v4.1.0) (2026-04-16)


### Features

* Add --json CLI flag for structured output ([0238b5a](https://github.com/dasch-swiss/sipi/commit/0238b5aaa5a62ec6dcd21937962796fa1721981f))


### Bug Fixes

* Resilient JPEG metadata + YCCK + CMYK APP14 + XMP scanner (DEV-6250, DEV-6257, DEV-6259) ([2360df8](https://github.com/dasch-swiss/sipi/commit/2360df864a78769a9fd9c7c5e2f6bf2e9e524b3f))
* Support 1-bit bilevel TIFF (DEV-6249) ([c45145c](https://github.com/dasch-swiss/sipi/commit/c45145cf4b373ad84852e1a47adea459ad9cf5f1))

## [4.0.1](https://github.com/dasch-swiss/sipi/compare/v4.0.0...v4.0.1) (2026-04-06)


### Bug Fixes

* Eliminate C++ exception-through-C UB in image format handlers ([94f45a2](https://github.com/dasch-swiss/sipi/commit/94f45a28c29618ce9477e7cf2ca16de4d9340890))

## [4.0.0](https://github.com/dasch-swiss/sipi/compare/v3.18.0...v4.0.0) (2026-03-22)


### ⚠ BREAKING CHANGES

* SipiImgInfo gains nc/bps fields (struct size change).

### Features

* Memory budget semaphore for concurrent image decode throttling ([afa682e](https://github.com/dasch-swiss/sipi/commit/afa682eff297ca6b7c41434f2eecc94b7be07f79))
* **metrics:** Add sipi_request_duration_seconds Prometheus histogram ([050f922](https://github.com/dasch-swiss/sipi/commit/050f922eb6260ec44a1a2c7b3a637821b5757925))
* OOM prevention, rate limiter, health endpoint, graceful shutdown ([45f3192](https://github.com/dasch-swiss/sipi/commit/45f31929b2894ed7c0c5c94b03e2f03d090770f8))
* **shttps:** Auto-detect thread count from CPU cores with container awareness ([85f05c1](https://github.com/dasch-swiss/sipi/commit/85f05c123131bfc38a5c28f24c06b9e37e1f9e95))


### Bug Fixes

* Input validation security hardening (R1-R10) ([76b0790](https://github.com/dasch-swiss/sipi/commit/76b07909d426769c0097c54e9d6d69202a353b5e))
* JPEG getDim memory leaks on error paths ([e963c89](https://github.com/dasch-swiss/sipi/commit/e963c89b1798463daeffbd4bb561a2ce8601895e))
* Memory safety for SipiImage and SipiFilenameHash (R11-R19) ([6f0da6c](https://github.com/dasch-swiss/sipi/commit/6f0da6ccb6186185f2a6554ec4ebc7543d7474db))
* PNG getDim returns 0x0 dimensions due to missing png_read_info ([955230e](https://github.com/dasch-swiss/sipi/commit/955230e7e3ab6036672ecbcd66a1f33aec031093))
* Queue liveness check, timeout validation, and unlimited queue default ([f4bba6a](https://github.com/dasch-swiss/sipi/commit/f4bba6a2f544a5366d96d3f1319c3b51ead2a7f4))
* Resolve all memory leaks detected by LeakSanitizer ([c092928](https://github.com/dasch-swiss/sipi/commit/c092928328fc91026b5e5d191169de5d81a9a17e))
* Resolve ASan/UBSan sanitizer findings (DEV-6038, DEV-6039, DEV-6040) ([90e076c](https://github.com/dasch-swiss/sipi/commit/90e076cdf8479fed16d9d867546d4470c89ed69e))
* Resolve heap-buffer-overflow, stack-use-after-scope, and UBSan findings ([a915198](https://github.com/dasch-swiss/sipi/commit/a9151984c2f3862692bab80987a1028b5516631e))
* **shttps:** Fix connection drops under concurrent load (DEV-6024) ([7a4459c](https://github.com/dasch-swiss/sipi/commit/7a4459ca28d8145f4344176ae2586e5031258829))
* **shttps:** Reverse poll loop iteration to prevent index-shifting bug (DEV-6024) ([5cf873f](https://github.com/dasch-swiss/sipi/commit/5cf873f04245ef5c5091a2e36985a1177978b78b))

## [3.18.0](https://github.com/dasch-swiss/sipi/compare/v3.17.2...v3.18.0) (2026-03-08)


### Features

* Add Zig toolchain for static binary distribution ([634ea81](https://github.com/dasch-swiss/sipi/commit/634ea81dfb04190039c433f0f1d37418fb234207))
* Eliminate docker-sipi-base, use ubuntu:24.04 ([684e9e3](https://github.com/dasch-swiss/sipi/commit/684e9e32bfac3d94b3e3be4f71af109d64795016))
* Embed libmagic database into binary for portable runtime ([0550157](https://github.com/dasch-swiss/sipi/commit/0550157cfcb2b0bd8d9b5f47bfccd960384c4c36))
* **metrics:** Add Prometheus cache metrics and /metrics endpoint ([4be73a3](https://github.com/dasch-swiss/sipi/commit/4be73a35a855d9abda89d195864645d72ca4d3e8))


### Bug Fixes

* Address review findings in magic handling and cross-compilation ([2510641](https://github.com/dasch-swiss/sipi/commit/2510641c4546d529be42f56f5ab591d31f8429fc))
* **build:** Kakadu libtool conflict with Nix and zig-clean scope ([eaffd72](https://github.com/dasch-swiss/sipi/commit/eaffd72fdd1fdf0b265be3354288176980b0b39c))
* **cache:** Rewrite cache management with LRU eviction, crash recovery, and config ([92429e1](https://github.com/dasch-swiss/sipi/commit/92429e1031735db3437b18b3ea1524c69696e0ed))
* HEAD response and LFS case sensitivity for e2e tests ([f3741ea](https://github.com/dasch-swiss/sipi/commit/f3741eae559c7334cf04a65a437c96fb4d89963a))

## [3.17.2](https://github.com/dasch-swiss/sipi/compare/v3.17.1...v3.17.2) (2026-03-03)


### Bug Fixes

* Correct degenerate-buffer fallback in bilinn from buf[n*c] to buf[c] ([5f996fe](https://github.com/dasch-swiss/sipi/commit/5f996fe9917060e57dbbc199b533bf04a4bca9c4))
* Prevent segfault in bilinn() bilinear interpolation during image scaling ([ba84f8b](https://github.com/dasch-swiss/sipi/commit/ba84f8b2eba1f7b560289cf2b503d96b18f4956c))

## [3.17.1](https://github.com/dasch-swiss/sipi/compare/v3.17.0...v3.17.1) (2026-02-27)


### Bug Fixes

* **ci:** Add retry logic for transient download and cache failures ([f8d7e03](https://github.com/dasch-swiss/sipi/commit/f8d7e03e6d1600ccbd54ee811f0b2a298738b2d5))
* **ci:** Expose GHA cache variables to Make for Docker layer caching ([bba3cc3](https://github.com/dasch-swiss/sipi/commit/bba3cc3211c59004c5b4b8b3f365c827431fc434))
* **ci:** Filter Docker Scout SARIF to critical/high severities ([3b27580](https://github.com/dasch-swiss/sipi/commit/3b27580dce3841717c6f369e207960ea5836e19d))
* **ci:** Remove invalid --release flag from sentry-cli debug-files upload ([bdc8c78](https://github.com/dasch-swiss/sipi/commit/bdc8c78aa46c73e51cd0764a7f4986e578edb09c))
* **test:** Replace fixed sleep with readiness polling in smoke tests ([ce4af8c](https://github.com/dasch-swiss/sipi/commit/ce4af8c317c15c16b81726ad4e2ea3b6500ac1f2))

## [3.17.0](https://github.com/dasch-swiss/sipi/compare/v3.16.3...v3.17.0) (2026-02-25)


### Features

* Add SARIF code scanning and SBOM generation to Scout integration ([6e89131](https://github.com/dasch-swiss/sipi/commit/6e89131a1c81dee6d6e91a27ae53cb5f452cd14c))
* Add server-side Sentry error capture and debug symbol upload ([9a7454a](https://github.com/dasch-swiss/sipi/commit/9a7454a1647f927773a7bb96a374b381cdffb09e))
* Improve error messages, CLI exit codes, and Sentry integration for image processing failures ([a49324b](https://github.com/dasch-swiss/sipi/commit/a49324bc8e991acdeefa4f2edabd020e220308bc))
* Integrate Docker Scout CVE scanning and rename aarch64 to arm64 ([9949313](https://github.com/dasch-swiss/sipi/commit/9949313686999c8932210cc4ed51527fdb4cc22c))
* Switch sentry-native to inproc backend for proper crash handling ([c62900d](https://github.com/dasch-swiss/sipi/commit/c62900d465520638c9d01c5a77c9d36a309f1ce4))


### Bug Fixes

* Address review feedback on Sentry integration ([0fe9b81](https://github.com/dasch-swiss/sipi/commit/0fe9b816c6dea6af57f667c350d5b65a7258f90e))
* Avoid full Docker rebuild for debug symbol extraction ([d86c67f](https://github.com/dasch-swiss/sipi/commit/d86c67fd447b7a44d5ed9fc25762545d35277f2a))
* Update zlib to 1.3.2 and fix setuptools on Ubuntu 24.04 ([e161b55](https://github.com/dasch-swiss/sipi/commit/e161b558c72ff65ff8cd722816f5d8cb21498a72))
* Upgrade setuptools in workflows before install-requirements ([f86180f](https://github.com/dasch-swiss/sipi/commit/f86180fba5c1eade5f1e230b6d2e05b22591a8c6))

## [3.16.3](https://github.com/dasch-swiss/sipi/compare/v3.16.2...v3.16.3) (2025-08-25)


### Bug Fixes

* Trigger release creation ([025b5f7](https://github.com/dasch-swiss/sipi/commit/025b5f71aa1a3fe3f874ff88387bfe67604a910a))

## [3.16.2](https://github.com/dasch-swiss/sipi/compare/v3.16.1...v3.16.2) (2025-07-25)


### Bug Fixes

* Streaming of large video files ([#486](https://github.com/dasch-swiss/sipi/issues/486)) ([7efc43e](https://github.com/dasch-swiss/sipi/commit/7efc43e547d2aee3b82a35708124b46665dbcbb7))

## [3.16.1](https://github.com/dasch-swiss/sipi/compare/v3.16.0...v3.16.1) (2025-07-23)


### Bug Fixes

* Also copy kakadu v8_5 for the upcoming PR ([04e456c](https://github.com/dasch-swiss/sipi/commit/04e456ce5a1174682eb61362f46b484e8fb0ffdf))
* Don't reinstall, if it's present (e.g. with updated CI base image) ([4159054](https://github.com/dasch-swiss/sipi/commit/41590548e6f7413467e7886cbd8810c54a0495af))
* Fix build on MacOS/CI ([#473](https://github.com/dasch-swiss/sipi/issues/473)) ([d06c2e9](https://github.com/dasch-swiss/sipi/commit/d06c2e994d95b766ce0fa9ce7c0eae3e7f5bbf54))
* Streaming of large files ([#481](https://github.com/dasch-swiss/sipi/issues/481)) ([1845a00](https://github.com/dasch-swiss/sipi/commit/1845a0047fc30bbcf095c8230226e3cb90fae6ce))
* TIFF tiled reading bug ([#478](https://github.com/dasch-swiss/sipi/issues/478)) ([a2ee1bf](https://github.com/dasch-swiss/sipi/commit/a2ee1bf4173d0c00e7705763bbd6e7f04050d344))

## [3.16.0](https://github.com/dasch-swiss/sipi/compare/v3.15.2...v3.16.0) (2025-03-24)


### Features

* Pyramidal TIFF support ([#466](https://github.com/dasch-swiss/sipi/issues/466)) ([807fccf](https://github.com/dasch-swiss/sipi/commit/807fccf0f62434679889ffd7a61b266eca81939a))


### Bug Fixes

* Drop "accepted connection from" from DEBUG to INFO ([e12f5e7](https://github.com/dasch-swiss/sipi/commit/e12f5e7a3f8cb8d9665f2878b37fa7eadd6947d0))

## [3.15.2](https://github.com/dasch-swiss/sipi/compare/v3.15.1...v3.15.2) (2025-03-19)


### Bug Fixes

* Logger.cpp in LuaServer.cpp, remove syslog, missing JSON quotes ([#470](https://github.com/dasch-swiss/sipi/issues/470)) ([b2899d7](https://github.com/dasch-swiss/sipi/commit/b2899d747b25b8476e1d35024c97f5d32b7c026f))

## [3.15.1](https://github.com/dasch-swiss/sipi/compare/v3.15.0...v3.15.1) (2025-03-14)


### Bug Fixes

* Parse percentages for IIIF size more carefully (DEV-4636) ([#468](https://github.com/dasch-swiss/sipi/issues/468)) ([d9a8490](https://github.com/dasch-swiss/sipi/commit/d9a849073a1fe3201077f44b814d5a2fd75cac3f))

## [3.15.0](https://github.com/dasch-swiss/sipi/compare/v3.14.0...v3.15.0) (2025-01-24)


### Features

* Return 404 for missing files (INFRA-735) ([#465](https://github.com/dasch-swiss/sipi/issues/465)) ([f54356f](https://github.com/dasch-swiss/sipi/commit/f54356f443f8bbb5dcb061384bd1aebbac4e85da))


### Bug Fixes

* Fix broken links and disable libwebp building its CLI tool ([#462](https://github.com/dasch-swiss/sipi/issues/462)) ([de9e393](https://github.com/dasch-swiss/sipi/commit/de9e393fe5e0b335751bd6166fefb5727723c675))
* Prevent exif nullptr (DEV-4521) ([#464](https://github.com/dasch-swiss/sipi/issues/464)) ([4231f00](https://github.com/dasch-swiss/sipi/commit/4231f00e839a7209f27be4548cd2ab16d387b25b))

## [3.14.0](https://github.com/dasch-swiss/sipi/compare/v3.13.0...v3.14.0) (2024-10-08)


### Features

* Respect X-Forwarded-Proto (DEV-3499) ([#453](https://github.com/dasch-swiss/sipi/issues/453)) ([d5eaab3](https://github.com/dasch-swiss/sipi/commit/d5eaab3bb36114759861c98a419ecca6955d41ce))
* Support IIIF Image's API HEAD requests (DEV-4072) ([#456](https://github.com/dasch-swiss/sipi/issues/456)) ([f3a9a96](https://github.com/dasch-swiss/sipi/commit/f3a9a9695a3c931e0d95144a07d1f7ba0bc182ae))
* Watermarks with alpha support and respect to ratio (DEV-4072) ([#458](https://github.com/dasch-swiss/sipi/issues/458)) ([da34f6a](https://github.com/dasch-swiss/sipi/commit/da34f6af0c51ceb3a47607e6f9ce9e2541a7497d))


### Bug Fixes

* Fix planar BigTIFF ingestion (DEV-3384) ([#455](https://github.com/dasch-swiss/sipi/issues/455)) ([83c2c94](https://github.com/dasch-swiss/sipi/commit/83c2c949fc9dc0c97fbd777f77a11adf38eac883))

## [3.13.0](https://github.com/dasch-swiss/sipi/compare/v3.12.3...v3.13.0) (2024-08-07)


### Features

* Allow YCbCr/RGB autoconvert, update libtiff (DEV-3863) ([#451](https://github.com/dasch-swiss/sipi/issues/451)) ([ccc68bc](https://github.com/dasch-swiss/sipi/commit/ccc68bc797e59befac48e29cab15b638a0b1f8aa))

## [3.12.3](https://github.com/dasch-swiss/sipi/compare/v3.12.2...v3.12.3) (2024-07-23)


### Bug Fixes

* Update ijg jpeg lib to v9f, partially fixes DEV-3474 ([#449](https://github.com/dasch-swiss/sipi/issues/449)) ([94e72fa](https://github.com/dasch-swiss/sipi/commit/94e72fa1cd3af41705e5f505e50bdaa28cee8b6b))

## [3.12.2](https://github.com/dasch-swiss/sipi/compare/v3.12.1...v3.12.2) (2024-04-07)


### Bug Fixes

* Triggering release creation ([8214a37](https://github.com/dasch-swiss/sipi/commit/8214a37fae0290eea6b29ff24760d288b0cb0baa))

## [3.12.1](https://github.com/dasch-swiss/sipi/compare/v3.12.0...v3.12.1) (2024-04-07)


### Bug Fixes

* Triggering release creation ([7434ed4](https://github.com/dasch-swiss/sipi/commit/7434ed42edb3869d542c96add443ae9af1d16b6d))

## [3.12.0](https://github.com/dasch-swiss/sipi/compare/v3.11.0...v3.12.0) (2024-04-07)


### Features

* Trying out release-please ([4ca8169](https://github.com/dasch-swiss/sipi/commit/4ca8169d87eee061350fed530e1318a2e21ed7f9))

## 3.11.0 (2024-04-05)


### Features

* -f option in curl, does not give output if status is not 200 ([d523213](https://github.com/dasch-swiss/sipi/commit/d523213f0eb657526f480dfe15bca6745709e716))
* Add better support for additional mimetypes, especially *.odd and *.rng ([#384](https://github.com/dasch-swiss/sipi/issues/384)) ([473474f](https://github.com/dasch-swiss/sipi/commit/473474f3d5f73f0f7fde99d2f5a9cce24f84d885))
* Add IIIF 3.0 support ([#324](https://github.com/dasch-swiss/sipi/issues/324)) ([d04725a](https://github.com/dasch-swiss/sipi/commit/d04725a55893e1326595a0560f6811c7923166e5))
* Add mimetype to knora json response ([#305](https://github.com/dasch-swiss/sipi/issues/305)) ([81ab98a](https://github.com/dasch-swiss/sipi/commit/81ab98a91ba7abc91fc33f4226034c2d1b9724aa))
* Add triggering of preflight script for non-image file types (DEV-1664) ([#381](https://github.com/dasch-swiss/sipi/issues/381)) ([b86428a](https://github.com/dasch-swiss/sipi/commit/b86428a952e7db85604da6a65e278a45d0bdddf4))
* Bash test script exit value ([876c270](https://github.com/dasch-swiss/sipi/commit/876c270c7485b0762fc0094d62664df1f7d289b5))
* Extend knora info for video ([#371](https://github.com/dasch-swiss/sipi/issues/371)) ([61cf681](https://github.com/dasch-swiss/sipi/commit/61cf68175e612b878f4cad92cfddec7df1acbc8a))
* Fixes, PDF, IIIF Auth, JPX compression parameters ([#290](https://github.com/dasch-swiss/sipi/issues/290)) ([6f46892](https://github.com/dasch-swiss/sipi/commit/6f46892fc36290cb1bd823b7b1b1659d4301fc79))
* Revert transparent JPG behavior ([#418](https://github.com/dasch-swiss/sipi/issues/418)) ([3675997](https://github.com/dasch-swiss/sipi/commit/3675997f9dc2249f48b9d7c54cef9ea162f697a6))
* Tilted images are imported without tilting (DEV-31) ([#374](https://github.com/dasch-swiss/sipi/issues/374)) ([8f65c6c](https://github.com/dasch-swiss/sipi/commit/8f65c6c451ddee5827bc209ae9399e12110294d7))
* Use gray instead of black values for transparent parts of image when returning as JPEG ([#412](https://github.com/dasch-swiss/sipi/issues/412)) ([f18dcfc](https://github.com/dasch-swiss/sipi/commit/f18dcfc28af5ce1f94a3762cc81cddda209a1461))


### Bug Fixes

* (SipiCache and Logger): added missing libraries ([6cb8a59](https://github.com/dasch-swiss/sipi/commit/6cb8a5955aa64715edf958b72dc02e24aebc660c))
* Add fix for [#83](https://github.com/dasch-swiss/sipi/issues/83). ([44b2090](https://github.com/dasch-swiss/sipi/commit/44b2090b8b60d3629adc91a1d041b85f612b8507))
* Add missing file ([#361](https://github.com/dasch-swiss/sipi/issues/361)) ([71ec889](https://github.com/dasch-swiss/sipi/commit/71ec88983c2eb97623f35e430646a6a48beb5388))
* Add missing files to Docker image ([#342](https://github.com/dasch-swiss/sipi/issues/342)) ([9960014](https://github.com/dasch-swiss/sipi/commit/996001478294909cd03241e34f293238e3beeaa6))
* Add missing include. ([e784251](https://github.com/dasch-swiss/sipi/commit/e784251452fffd9053c9034193a63534811c7c35))
* Better handling of missing sidecar files ([#376](https://github.com/dasch-swiss/sipi/issues/376)) ([bed711f](https://github.com/dasch-swiss/sipi/commit/bed711fc9aa41fea095f139c301f9dfd8f5185fc))
* Correct typo in favicon route. ([7a2e320](https://github.com/dasch-swiss/sipi/commit/7a2e3200de16da832f24d969536b19ed9a42d5c9))
* Crashing on jp2 decompression ([#407](https://github.com/dasch-swiss/sipi/issues/407)) ([93308c2](https://github.com/dasch-swiss/sipi/commit/93308c2bdd2d68cdea02808eda788833d2aaf83d))
* Docs deployment ([e884677](https://github.com/dasch-swiss/sipi/commit/e884677db6cec16b34e2b923b042d2031421b35a))
* Don’t make detached threads, and improve signal handling ([#93](https://github.com/dasch-swiss/sipi/issues/93)). ([4365500](https://github.com/dasch-swiss/sipi/commit/4365500644cc1cceb816d04d9711007e60364beb))
* Fix deadlock on mutex protecting thread_ids. ([f42d3e3](https://github.com/dasch-swiss/sipi/commit/f42d3e3620df18ecd5a65830f0a295736d188d89))
* Fix memory leak in SipiImage. ([aadc63e](https://github.com/dasch-swiss/sipi/commit/aadc63eaa9b89898b1ae85a421087792bcce610a))
* IIIF URL redirection ([#417](https://github.com/dasch-swiss/sipi/issues/417)) ([1905bf2](https://github.com/dasch-swiss/sipi/commit/1905bf2fe740960cc87dae23f71c8268ccde8fd9))
* Incorrect error message ([#383](https://github.com/dasch-swiss/sipi/issues/383)) ([94b50f9](https://github.com/dasch-swiss/sipi/commit/94b50f9985ab6582cfc234e229f37f1344d49c67))
* Invalid watermark crashes sipi ([#406](https://github.com/dasch-swiss/sipi/issues/406)) ([39dc0e8](https://github.com/dasch-swiss/sipi/commit/39dc0e87b5fcdebee162cf251736e3ee9a5229af))
* Issues ([#328](https://github.com/dasch-swiss/sipi/issues/328)) ([2d78c55](https://github.com/dasch-swiss/sipi/commit/2d78c553cfb7465ebdca0abf195f6cebdf1870a9))
* Kakadu error ([#341](https://github.com/dasch-swiss/sipi/issues/341))(DSP-1247) ([d0bc37a](https://github.com/dasch-swiss/sipi/commit/d0bc37a047a8ee75160d490ac9f6903470547c2e))
* Knora upload scripts and configs ([#174](https://github.com/dasch-swiss/sipi/issues/174)) ([51054d0](https://github.com/dasch-swiss/sipi/commit/51054d02f278320d5694b3063070b5dc1370e3c1))
* **knora.json:** Return origin instead of wildcard (DEV-318) ([#369](https://github.com/dasch-swiss/sipi/issues/369)) ([3b73ff7](https://github.com/dasch-swiss/sipi/commit/3b73ff726e987a675a6927c63b6b23e3c75584ea))
* Make signal handler code asynchronous-safe. ([8cc43b3](https://github.com/dasch-swiss/sipi/commit/8cc43b39b5b6177119834c655c8db3d6b5fffe66))
* Memory leaks (DEV-237) ([#365](https://github.com/dasch-swiss/sipi/issues/365)) ([c3b9b35](https://github.com/dasch-swiss/sipi/commit/c3b9b3519bfb2b3d10f50c72c2e81a8d2b506f35))
* Palette color tiffs now read correctly ([83037c5](https://github.com/dasch-swiss/sipi/commit/83037c581acfbcfd3552c7fb9f7d3be20986c427))
* Palette color tiffs now read correctly ([#253](https://github.com/dasch-swiss/sipi/issues/253)) ([83037c5](https://github.com/dasch-swiss/sipi/commit/83037c581acfbcfd3552c7fb9f7d3be20986c427))
* Parse url crash ([#340](https://github.com/dasch-swiss/sipi/issues/340)) (DSP-1247) ([e710237](https://github.com/dasch-swiss/sipi/commit/e7102379998ab38c0b6162fa4c4a3599eb328cb2))
* Printed version strings (DSP-687) ([#332](https://github.com/dasch-swiss/sipi/issues/332)) ([9778b19](https://github.com/dasch-swiss/sipi/commit/9778b19aa27536dd4b11574d6d62fb1824d021cf))
* Replace custom HTTP client code with libcurl. ([fb1bdfc](https://github.com/dasch-swiss/sipi/commit/fb1bdfced897b7e1e5268b8c2f9fe2859b83ae14))
* Return Internal Server Error if Lua function has invalid return value. ([2c72dde](https://github.com/dasch-swiss/sipi/commit/2c72dde59a847b43465f347d3d93aab4c238b5c2))
* Small bugfix in CMakeLists.txt ([cf1c275](https://github.com/dasch-swiss/sipi/commit/cf1c2758c385538f6a6f331780b6c0045a7aa24d))
* Support for grayscale jpegs ([#410](https://github.com/dasch-swiss/sipi/issues/410)) ([fd63dd0](https://github.com/dasch-swiss/sipi/commit/fd63dd089db12c7344d2665dff63b6d2586ae1de))
* Support for TIFF with CMYK and alpha channel ([#408](https://github.com/dasch-swiss/sipi/issues/408)) ([c0cc033](https://github.com/dasch-swiss/sipi/commit/c0cc033cab0cb8d86a267f532eff9e13eff6fee8))
* Take watermark into account when caching ([#421](https://github.com/dasch-swiss/sipi/issues/421)) ([18788b2](https://github.com/dasch-swiss/sipi/commit/18788b20e19d0fe7af97f6e791822f5d503d2650))
* Uploading PNGs with transparency crashes SIPI ([#375](https://github.com/dasch-swiss/sipi/issues/375)) ([01104b5](https://github.com/dasch-swiss/sipi/commit/01104b520ea5e360154bec5b2f4b8080a264e3e4))
* Use PATCH_COMMAND instead of UPDATE_COMMAND so the same patch isn't applied twice ([#53](https://github.com/dasch-swiss/sipi/issues/53)) ([1f9695a](https://github.com/dasch-swiss/sipi/commit/1f9695a8a0ee7b21090699e8ea3750694fa09d50))
* Use prefix if provided in sipi.init-knora-test.lua. ([4b99130](https://github.com/dasch-swiss/sipi/commit/4b99130f3d585e1928a12e85d6fe699c653608aa))
* Use RAII to manage libcurl connections. ([87f1366](https://github.com/dasch-swiss/sipi/commit/87f13664098fb18a22ce05c1d10416efe5848930))
* Watermark support ([#405](https://github.com/dasch-swiss/sipi/issues/405)) ([b7abe85](https://github.com/dasch-swiss/sipi/commit/b7abe857cc7acc6a9820ed327c5e913389d72a44))


### Miscellaneous Chores

* Release 3.11.0 ([f76eebb](https://github.com/dasch-swiss/sipi/commit/f76eebb7a91bd4722f3be5419c9f897336247abb))
