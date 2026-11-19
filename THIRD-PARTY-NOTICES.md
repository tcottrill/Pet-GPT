# Third-party notices

Pet-GPT itself is Copyright (C) 2026 Tim Cottrill and is released under GPL-3.0-or-later (see README.md). This file lists the third-party code that is in the source tree or linked into the program, with the licence and copyright text taken from the file headers. Items marked "not built" are in the tree but are not part of the CMake build (`CMakeLists.txt`). Problems that need the author's decision are collected in the last section.

No Commodore ROM images or commercial software are part of this project or its releases.

| Component | Path | Licence | Built |
|---|---|---|---|
| Thomas Skibo's PET emulator sources | `petemu/petsrc/` (4 files) | BSD 2-clause | yes |
| GLEW 2.3.4 | `petemu/thirdparty/glew.c`, `glew.h`, `wglew.h`, `GL/eglew.h` | Modified BSD, Mesa MIT, Khronos MIT | yes |
| stb_image 2.08, stb_image_write 1.00 | `petemu/thirdparty/stb_image*.h` | public domain | not built |
| miniz 1.15 | `petemu/thirdparty/miniz.c`, `miniz.h` | Unlicense | not built |
| AAE 6502 core | `petemu/cpu_cores/cpu_6502.cpp`, `cpu_6502.h` | Unlicense | yes |
| `deftypes.h` | `petemu/cpu_cores/deftypes.h` | none stated | yes |
| iniFile | `petemu/sys_general/iniFile.cpp` | Unlicense | yes |
| Erin Catto's math header | `petemu/sys_general/MathUtils.h` | permissive notice | not built |
| wxWidgets 3.3.3 | fetched at build time | wxWindows Library Licence 3.1 | yes (GUI) |

Code under `petemu/sys_audio`, `petemu/sys_input` and `petemu/sys_general/sys_log.*` is the author's own, copied from the author's AAE project, and is GPL-3.0-or-later; see `petemu/sys_audio/REUSE.md`. The headers carry no third-party notice other than the placeholders listed under "Needs the author's attention".

## Thomas Skibo's PET emulator sources (BSD 2-clause)

Files, each carrying this notice in its header:

- `petemu/petsrc/pet2001video.cpp`
- `petemu/petsrc/pet_kbd_input.cpp` (marked "Original code")
- `petemu/petsrc/pet_machine.cpp`
- `petemu/petsrc/pet_mem.cpp`

Pet-GPT's PET hardware code started from Thomas Skibo's JavaScript Commodore PET emulator (see the README acknowledgements) and has been substantially rewritten since. The notice, as it appears in those files:

```text
Copyright (c) 2012,2014 Thomas Skibo.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY AUTHOR AND CONTRIBUTORS ``AS IS'' AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
SUCH DAMAGE.
```

## GLEW (OpenGL Extension Wrangler Library)

- Files: `petemu/thirdparty/glew.c`, `glew.h`, `wglew.h`, `GL/eglew.h` (`GL/glew.h` is a one-line forwarding header written for this project).
- Version: 2.3.4 (`GLEW_VERSION_MAJOR/MINOR/MICRO` in `glew.h`).
- Licences: Modified BSD (GLEW), MIT (Mesa 3-D, in `glew.h` and `eglew.h`), MIT (Khronos Group, in `glew.h`, `wglew.h`, `eglew.h`).
- Copyright, as in the file headers:
  - Copyright (C) 2008-2019, Nigel Stewart `<nigels[]users sourceforge net>` (`GL/eglew.h`: 2008-2025, `<nigels[]nigels com>`)
  - Copyright (C) 2002-2008, Milan Ikits `<milan ikits[]ieee org>`
  - Copyright (C) 2002-2008, Marcelo E. Magallon `<mmagallo[]debian org>`
  - Copyright (C) 2002, Lev Povalahev
  - Copyright (C) 1999-2007 Brian Paul (Mesa)
  - Copyright (c) 2007 The Khronos Group Inc.

GLEW notice:

```text
The OpenGL Extension Wrangler Library
Copyright (C) 2008-2019, Nigel Stewart <nigels[]users sourceforge net>
Copyright (C) 2002-2008, Milan Ikits <milan ikits[]ieee org>
Copyright (C) 2002-2008, Marcelo E. Magallon <mmagallo[]debian org>
Copyright (C) 2002, Lev Povalahev
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice,
  this list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.
* The name of the author may be used to endorse or promote products
  derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
THE POSSIBILITY OF SUCH DAMAGE.
```

Mesa 3-D graphics library, Version 7.0:

```text
Copyright (C) 1999-2007  Brian Paul   All Rights Reserved.

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
BRIAN PAUL BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

Khronos Group:

```text
Copyright (c) 2007 The Khronos Group Inc.

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and/or associated documentation files (the
"Materials"), to deal in the Materials without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Materials, and to
permit persons to whom the Materials are furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Materials.

THE MATERIALS ARE PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
MATERIALS OR THE USE OR OTHER DEALINGS IN THE MATERIALS.
```

## stb_image and stb_image_write (not built)

- Files: `petemu/thirdparty/stb_image.h` (v2.08), `stb_image_impl.cpp` (a two-line wrapper), `stb_image_write.h` (v1.00).
- Licence: public domain. stb_image.h states: "This software is in the public domain. Where that dedication is not recognized, you are granted a perpetual, irrevocable license to copy, distribute, and modify this file as you see fit."
- Copyright holders: none claimed. Authorship credit: Sean Barrett (2010-2015 for stb_image_write.h) and the contributors listed in the stb_image.h header (including Nicolas Schulz, Jonathan Dummer, Jean-Marc Lienher, Tom Seddon, Thatcher Ulrich, Ken Miller).

## miniz (not built)

- Files: `petemu/thirdparty/miniz.c`, `miniz.h` (a wrapper that includes `miniz.c`).
- Version: v1.15 (r4), last updated Oct. 13, 2013.
- Licence: Unlicense (public domain dedication).
- Author named in the header: Rich Geldreich `<richgel99@gmail.com>`.

```text
This is free and unencumbered software released into the public domain.

Anyone is free to copy, modify, publish, use, compile, sell, or
distribute this software, either in source code form or as a compiled
binary, for any purpose, commercial or non-commercial, and by any
means.

In jurisdictions that recognize copyright laws, the author or authors
of this software dedicate any and all copyright interest in the
software to the public domain. We make this dedication for the benefit
of the public at large and to the detriment of our heirs and
successors. We intend this dedication to be an overt act of
relinquishment in perpetuity of all present and future rights to this
software under copyright law.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.

For more information, please refer to <http://unlicense.org/>
```

## AAE 6502 CPU core

- Files: `petemu/cpu_cores/cpu_6502.cpp`, `cpu_6502.h`.
- Licence: The Unlicense ("This file is part of the AAE project and is released under The Unlicense", see <http://unlicense.org/>).
- Origins stated in `cpu_6502.h`: original 6502 core by Neil Bradley, adapted from unnamed web sources; ADC and SBC algorithms from M.A.M.E. (c) 1998-2000 Juergen Buchmueller; timing tables and stack logic adapted from FakeNes; refactor and extensions by TC (2015-2025). See "Needs the author's attention".

## iniFile

- File: `petemu/sys_general/iniFile.cpp` (`iniFile.h` has no header).
- Licence: The Unlicense: "This is free and unencumbered software released into the public domain." (full text in the file; see <https://unlicense.org/>). No copyright holder is named. The header notes: "Some code below was written with the assistance of ChatGPT".

## Erin Catto's math header (not built)

- File: `petemu/sys_general/MathUtils.h` (listed in `petemu.filters` only; no source includes it).
- Licence: permissive notice, no name given.

```text
Copyright (c) 2006-2007 Erin Catto http://www.gphysics.com

Permission to use, copy, modify, distribute and sell this software
and its documentation for any purpose is hereby granted without fee,
provided that the above copyright notice appear in all copies.
Erin Catto makes no representations about the suitability
of this software for any purpose.
It is provided "as is" without express or implied warranty.
```

## wxWidgets

- What: the cross-platform GUI toolkit used by the frontend; not in the tree. CMake downloads the release archive at configure time (`cmake/Dependencies.cmake`), or uses a local checkout given as `-DPET_WX_SOURCE=`.
- Version fetched: 3.3.3 (`https://github.com/wxWidgets/wxWidgets/releases/download/v3.3.3/wxWidgets-3.3.3.tar.bz2`). docs/porting.md records verification runs with a local 3.3.4 checkout.
- Licence: wxWindows Library Licence, Version 3.1 (LGPL 2 or later with an exception that permits distributing binary forms under terms of your choice). The licence text and copyright holders are in the fetched sources, `docs/licence.txt` and `docs/lgpl.txt`; they are not reproduced here because they are not part of this repository.
- Release packages that include a wxWidgets-linked binary should include that text.

## Needs the author's attention

Placeholder or inconsistent headers. Nothing below was changed or interpreted here.

1. `petemu/sys_audio/mixer.cpp` (lines 119-136), `petemu/sys_general/sys_log.cpp` (lines 78-95), `petemu/sys_general/sys_log.h` (lines 59-76): the GPL header names another project and a placeholder. Exact text: "This file is part of GameEngine Alpha.", "<Project Name> is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License...", "along with GameEngine Alpha.  If not, see <https://www.gnu.org/licenses/>.", "Copyright (C) 2022-2025  Tim Cottrill" (`mixer.cpp`) and "Copyright (C) 2012-2025  Tim Cottrill" (`sys_log.*`), "SPDX-License-Identifier: GPL-3.0-or-later". `sys_log.cpp` also says `LOG_INFO("Hello from %s", "GameEngine Alpha");` in its usage notes.
2. `petemu/sys_general/shader_util.h`: header reads "Game Engine Alpha - Generic Module ... This library is part of the **Game Engine Alpha** project", then a GPL v3 or later notice with no copyright holder and no year. Not built.
3. `petemu/system/portable_paths.cpp` line 1: "Copyright Tim Cottrill 2025; portable host paths, 2026. Public domain." The rest of the project is GPL-3.0-or-later.
4. `petemu/cpu_cores/cpu_6502.cpp` and `.h`: the file is released under The Unlicense, but `cpu_6502.h` says the ADC/SBC algorithms come from M.A.M.E. (c) 1998-2000 Juergen Buchmueller, the timing tables and stack logic from FakeNes, and the original core is by Neil Bradley "adapted from unnamed web sources". The licences of those sources are not stated in the files.
5. `petemu/cpu_cores/deftypes.h`: "Originally from RAINE" with no licence or copyright line.
6. `petemu/petsrc/pet2001ieee_ioport.cpp` (line 13): "This was originally based on Thomas Skibo's code but has moved to a point where I consider it to be it's own source, if you can call it that with AI generated." The file has no Skibo notice. Likewise `petemu/petsrc/pet2001video.h` says "ported from pet2001video.js" (the .cpp carries the notice; the header does not). If any code in the other `petsrc` files (for example `pet2001io.*`, `pet2001ieee*.cpp`) derives from Skibo's sources, the BSD notice has to be added to them too.
7. `petemu/sys_audio/linux/alsa_backend.cpp` (about lines 153, 439, 861): "ported from the video player's ..." and "Ported from the sibling video player (mp4_av_player audio_output_pipewire.cpp)". Presumably the author's own project; confirm it is under the same licence.
8. `petemu/sys_general/iniFile.cpp`: Unlicense with no named holder, and a note that part of the code was written with ChatGPT; `iniFile.h` and most other files in `petemu/petsrc`, `petemu/wx`, `petemu/emulator.*` have no licence header at all. If the project is to be GPL-3.0-or-later throughout, a header or an SPDX line in those files would remove doubt.
9. `petemu/thirdparty/GL/eglew.h` says 2008-2025 (`<nigels[]nigels com>`) while `glew.c`, `glew.h`, `wglew.h` say 2008-2019 (`<nigels[]users sourceforge net>`), and `glew.h` carries a comment block "VERSION 2.2.0 / VERSION_MINOR 2" beside `GLEW_VERSION_MINOR 3`, `GLEW_VERSION_MICRO 4`. The bundled GLEW files may come from different GLEW releases; the licences are the same.
10. `petemu/wx/app.cpp` About box says: "GNU General Public License, version 3.\nSee LICENSE in the distribution." and `SetCopyright("Pet-GPT contributors")`. The README says "version 3 ... or (at your option) any later version", and the headers say "or later". There is no LICENSE file in the tree yet.
