"""Reproducible 2K copy of Intel's New Sponza glTF release.

Copies each .gltf, its .bin and credits_license.txt byte for byte, and writes every image the
document references at half size under the same relative path, so no uri changes. One 2x2 box
step per image, by role:
  srgb   (baseColor, emissive): decode sRGB to linear, average, encode sRGB; alpha averages linearly.
  linear (metallicRoughness, occlusion, unknown): average the stored values.
  normal: decode to vectors, average, renormalize, encode.
--flip-normal-green negates the decoded Y of normal maps (DirectX to glTF's +Y-up convention).
source.json records the input and output SHA-256 of every file, the command and tool versions.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import shutil
import sys
import urllib.parse
import zlib

import numpy as np
from PIL import Image, __version__ as pillow_version

SOURCE_SIZE, OUTPUT_SIZE = 4096, 2048


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as file:
        for block in iter(lambda: file.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def relative_uri(uri):
    path = PurePosixPath(urllib.parse.unquote(uri))
    if uri.startswith('data:') or path.is_absolute() or '..' in path.parts or ':' in uri:
        raise ValueError(f'uri must stay inside the source directory: {uri}')
    return Path(*[part for part in path.parts if part != '.'])


def image_roles(document):
    roles = {}
    def add(info, role):
        source = document['textures'][info['index']]['source']
        if roles.setdefault(source, role) != role:
            raise ValueError(f'image {source} is used both as {roles[source]} and {role}')
    for material in document.get('materials', []):
        pbr = material.get('pbrMetallicRoughness', {})
        if 'baseColorTexture' in pbr: add(pbr['baseColorTexture'], 'srgb')
        if 'metallicRoughnessTexture' in pbr: add(pbr['metallicRoughnessTexture'], 'linear')
        if 'emissiveTexture' in material: add(material['emissiveTexture'], 'srgb')
        if 'occlusionTexture' in material: add(material['occlusionTexture'], 'linear')
        if 'normalTexture' in material: add(material['normalTexture'], 'normal')
    return [roles.get(index, 'linear') for index in range(len(document.get('images', [])))]


def box(values):
    return (values[0::2, 0::2] + values[1::2, 0::2] + values[0::2, 1::2] + values[1::2, 1::2]) * .25


def quantize(values):
    return np.clip(np.floor(values * 255 + .5), 0, 255).astype(np.uint8)


def downscale(source, output, role, flip_green):
    with Image.open(source) as image:
        if image.mode not in ('RGB', 'RGBA') or image.size != (SOURCE_SIZE, SOURCE_SIZE):
            raise ValueError(f'{source}: expected an 8-bit RGB(A) {SOURCE_SIZE}x{SOURCE_SIZE} PNG, got {image.mode} {image.size}')
        mode = image.mode
        pixels = np.asarray(image, dtype=np.float64) / 255
    color = pixels[..., :3]
    if role == 'srgb':
        linear = np.where(color <= .04045, color / 12.92, ((color + .055) / 1.055) ** 2.4)
        reduced = box(linear)
        color = np.where(reduced <= .0031308, reduced * 12.92, 1.055 * reduced ** (1 / 2.4) - .055)
    elif role == 'normal':
        vectors = box(color * 2 - 1)
        if flip_green:
            vectors[..., 1] = -vectors[..., 1]
        length = np.linalg.norm(vectors, axis=-1, keepdims=True)
        vectors = np.where(length > 1e-12, vectors / np.where(length > 1e-12, length, 1), (0, 0, 1))
        color = vectors * .5 + .5
    else:
        color = box(color)
    channels = [color] + ([box(pixels[..., 3:4])] if mode == 'RGBA' else [])
    result = quantize(np.concatenate(channels, axis=-1))
    output.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(result).save(output, format='PNG', compress_level=9)


def convert(document_path, destination, flip_green, jobs):
    source_root = document_path.parent
    document = json.loads(document_path.read_bytes())
    files = [(Path(document_path.name), 'verbatim')]
    files += [(relative_uri(buffer['uri']), 'verbatim') for buffer in document.get('buffers', [])]
    files += [(Path('credits_license.txt'), 'verbatim')]
    roles = image_roles(document)
    images = [(relative_uri(image['uri']), role) for image, role in zip(document.get('images', []), roles)]
    if len({path for path, _ in images}) != len(images):
        raise ValueError(f'{document_path}: two images share one uri')
    destination.mkdir(parents=True, exist_ok=True)
    for relative, _ in files:
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source_root / relative, target)
    with concurrent.futures.ProcessPoolExecutor(max_workers=jobs) as pool:
        work = [pool.submit(downscale, source_root / relative, destination / relative, role, flip_green) for relative, role in images]
        for future in work:
            future.result()
    records = []
    for relative, role in files + images:
        source, output = source_root / relative, destination / relative
        record = {'path': relative.as_posix(), 'role': role,
                  'source_bytes': source.stat().st_size, 'source_sha256': sha256(source),
                  'output_bytes': output.stat().st_size, 'output_sha256': sha256(output)}
        if role == 'verbatim' and record['source_sha256'] != record['output_sha256']:
            raise RuntimeError(f'{relative} changed while copying')
        records.append(record)
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('source', type=Path, help='glTF document inside the original release folder')
    parser.add_argument('destination', type=Path, help='new output folder; existing files are replaced')
    parser.add_argument('--flip-normal-green', action='store_true')
    parser.add_argument('--jobs', type=int, default=max(1, min(8, (os.cpu_count() or 2) // 2)))
    arguments = parser.parse_args()
    records = convert(arguments.source.resolve(), arguments.destination.resolve(), arguments.flip_normal_green, arguments.jobs)
    manifest = {
        'source': 'Intel New Sponza (2022), https://www.intel.com/content/www/us/en/developer/topic-technology/graphics-research/samples.html',
        'license': 'CC BY 4.0; see credits_license.txt',
        'command': 'python tools/downscale-sponza/downscale.py <source.gltf> <destination>' + (' --flip-normal-green' if arguments.flip_normal_green else ''),
        'filter': f'2x2 box, {SOURCE_SIZE} to {OUTPUT_SIZE}: srgb in linear light, linear values directly, normals renormalized',
        'normal_green': 'flipped (DirectX to glTF +Y up)' if arguments.flip_normal_green else 'unchanged',
        'tools': {'python': platform.python_version(), 'numpy': np.__version__, 'pillow': pillow_version,
                  'zlib': zlib.ZLIB_RUNTIME_VERSION, 'png_compress_level': 9},
        'files': records,
    }
    (arguments.destination / 'source.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8', newline='\n')


if __name__ == '__main__':
    sys.exit(main())
