# micro-md2pdf

![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue)
![C99](https://img.shields.io/badge/C-C99-blue)
![Multi-Arch](https://img.shields.io/badge/Arch-amd64%20%7C%20arm64%20%7C%20riscv64-orange)
![Builds: Reproducible](https://img.shields.io/badge/Builds-Reproducible-green)

**Ultra-compact, multi-architecture Markdown to PDF converter written in pure C99.**

`micro-md2pdf` is engineered for extreme efficiency, architecture portability, and bit-for-bit reproducible builds. It parses a robust subset of GitHub Flavored Markdown (GFM) and renders it directly into clean, high-resolution PDF documents without requiring heavy external runtimes such as Node.js, Python, Electron, or a full LaTeX distribution.

---

### 📜 AUTHOR'S MANIFESTO ON HYBRID ENGINEERING (AI COMPILATION ENGINE)
> This software was forged through a highly disruptive prompting process utilizing the web-chat interface of Free-Tier Large Language Models (LLMs) under the unyielding direction, architectural vision, and design constraints of its human author. 
> 
> For all legal intents and purposes, the human author asserts complete intellectual authorship and technical control over the system architecture, modular layout, and rigid quality control. The AI tool functioned strictly as an advanced boilerplate compilation engine; the final engineering grid belongs to real human grit.

---

## 🚀 Key Features

- **Ultra-Compact Static Binary**:
  - Compiled using GCC/Musl with `-Os`, `-fomit-frame-pointer`, `-ffunction-sections`, `-fdata-sections`, and `--gc-sections` for aggressive dead-code elimination.
  - Final executable size is ~1.43 MB (100% static, zero shared library dependencies).
- **Architecture Portability**:
  - Multi-arch builds for `amd64`, `arm64`, and `riscv64` using Docker and `qemu-user-static`.
- **Full Networking & Image Stack**:
  - Embedded image decoders: JPEG, PNG, WebP (`libwebp`), and AVIF (`dav1d` + `libavif`).
  - Remote HTTP/HTTPS image fetching powered by **MbedTLS 3.6 (TLS 1.3 support)** over POSIX sockets.
- **Full Color Emoji Support**:
  - Dynamically builds a Twemoji atlas from the Markdown source and renders 100% accurate color emojis inline, bypassing standard PDF font limitations.
- **Smart Badge Engine**:
  - Auto-detects `shields.io` SVG URLs and fetches them as high-res PNGs.
  - Renders multiple badges inline horizontally, scaling them perfectly to match standard text height.
- **Reproducible Builds**:
  - Enforces `SOURCE_DATE_EPOCH` and `ZERO_AR_DATE` to ensure identical binary output across build environments.
- **Advanced Markdown Parsing**:
  - Nested parentheses in URLs (e.g. Wikipedia links: `Avida_(software)`).
  - GFM bare URL autolinking with trailing punctuation stripping.
  - Dynamic table column width calculation and line wrapping to prevent PDF layout overflow.

---

## 📊 Benchmarks & Comparison

`micro-md2pdf` is designed as a spartan alternative to bloated document processing toolchains.

| Metric / Feature | micro-md2pdf | WeasyPrint | Pandoc + LaTeX |
| :--- | :--- | :--- | :--- |
| **Binary / Install Size** | **~1.43 MB** | ~50+ MB | >100 MB |
| **Language** | Pure C99 | Python | Haskell |
| **Runtime Dependencies** | **None (100% Static musl)** | Python, Cairo, Pango | Haskell Engine, TeX Live |
| **Execution Time** | **< 50 ms** | ~1.5 - 3.0 s | > 2.0 s |
| **RAM Footprint** | **< 10 MB** | > 120 MB | > 250 MB |
| **TLS / Remote Fetching**| Built-in MbedTLS (TLS 1.3) | Python `requests` | External `curl` / `wget` |
| **Architecture Portability**| `amd64`, `arm64`, `riscv64` | Requires Python env | Requires TeX per arch |

---

## 📦 Quick Start

### 1. Clone Repository
```bash
git clone https://github.com/johna124/micro-md2pdf.git
cd micro-md2pdf
```

### 2. Build Binaries
The automated `compile.sh` build script manages dependency isolation using Docker + Alpine Linux.

```bash
chmod +x compile.sh

# Target specific architecture
./compile.sh amd64      # x86_64 Linux / Servers
./compile.sh arm64      # Raspberry Pi 4/5, Apple Silicon, ARM Cloud
./compile.sh riscv64    # Modern RISC-V Single Board Computers

# Build all supported architectures in one pass
./compile.sh all
```

*Note: Multi-architecture cross-compilation requires QEMU binfmt support on the host system:*
```bash
docker run --privileged --rm tonistiigi/binfmt --install all
```

### 3. Usage
```bash
# Convert markdown to PDF (auto-generates output name if omitted)
./micro-md2pdf-amd64 input.md output.pdf
```

---

## 🛠️ Build Toolchain & Dependencies

### Docker / Alpine Build Environment (`compile.sh`)
When building with Strategy A (Docker + Alpine), all dependencies are compiled from source inside an isolated container with maximum feature trimming:

- **Build Tools**: `build-base`, `cmake`, `git`, `meson`, `ninja`, `pkgconfig`, `nasm`, `perl`, `py3-jsonschema`, `py3-jinja2`.
- **MbedTLS (`mbedtls-3.6` branch)**: Embedded TLS 1.3 implementation for HTTPS image fetching.
- **libwebp**: Stripped decoder-only build with CLI tools disabled.
- **dav1d**: 8-bit decoder only, ASM/SIMD disabled.
- **libavif**: Decoder-only mode (`AVIF_CODEC_AOM=OFF`, `AVIF_BUILD_APPS=OFF`).

---

## 📝 Supported Markdown Features

| Feature | Syntax Example | Notes |
| :--- | :--- | :--- |
| **Headings** | `# H1` through `###### H6` | H1 and H2 feature decorative underlines |
| **Inline Formatting**| `*italic*`, `**bold**`, `` `code` `` | Full inline style stack support |
| **Links** | `[Text](https://site.com)` | Supports nested parens & bare URLs |
| **Emojis** | `🚀`, `✅`, `⚠️` | Rendered as inline color images (Twemoji) |
| **Lists** | `- Item 1` or `* Item 1` | Bulleted list formatting |
| **Blockquotes** | `> Referenced text` | Indented quote styling |
| **Tables** | `\| Col 1 \| Col 2 \|` | Dynamic column width calculation |
| **Images & Badges** | `![alt](url)`, `[![alt](url)](link)` | JPG, PNG, WebP, AVIF, shields.io badges (inline) |
| **Horizontal Rule** | `---`, `***`, or `___` | Page break / section separators |

## 🚫 Lack of Features & Limitations

- **Limited Native SVG Support**: Generic `.svg` files are not rasterized natively. However, `shields.io` badge URLs are auto-converted to high-res PNGs for perfect rendering.
- **No Automatic PDF Text Refinement**: Fonts are limited to the PDF Base-14 (Helvetica, Courier) to guarantee the ~1.4 MB binary size. No external TrueType fonts are embedded.

---

## ⚙️ Architecture Support Matrix

| Target Arch | Container Platform | Target Hardware | Status |
| :--- | :--- | :--- | :--- |
| `amd64` | `linux/amd64` | Intel/AMD x86_64 Servers & Desktops | ✅ Production Ready |
| `arm64` | `linux/arm64` | Raspberry Pi 4/5, AWS Graviton, Apple M-Series | ✅ Production Ready |
| `riscv64` | `linux/riscv64` | Milk-V Pioneer, StarFive VisionFive 2 | ✅ Production Ready |

---

## 📜 License

Distributed under the **GNU General Public License v3.0**. See the [LICENSE](LICENSE) file for complete details.

### Third-Party Components
This project embeds or relies on the following open-source software:
- **MbedTLS (v3.6.0)**: Provides TLS 1.3 encryption and POSIX socket networking.
- **libwebp**: Used for WebP image decoding.
- **dav1d**: AV1 video/image decoder backend.
- **libavif**: Used for AVIF container parsing and image decoding.
- **Docker & Alpine Linux**: Utilized exclusively within the multi-arch build pipeline.

