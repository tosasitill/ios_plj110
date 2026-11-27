#!/usr/bin/env python3
"""Read-only 20H392 auxC provenance slices, not an alpha decoder.

Fixed NEW ranges only; the previous alpha helper/caller/subtype/stubs are not
re-read. Qualification compares extracted text to the retained root clone's
cache mappings. Output contains exact bytes/direct BLs; indirect dispatch and
opaque subtype framing are NOT inferred. No mount, device, runtime execution,
build verification or auxiliary hardware admission.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
GROUPS = {
    'aux-origin': (
        '.local/apple/dsc-20H392/System/Library/PrivateFrameworks/CMPhoto.framework/CMPhoto',
        '8c636e49-40af-38ac-87e6-4943a376eed2', (
            (0x1A5DCA19C, 404, '__copyAuxiliaryImageTypeInfo'),
            (0x1A5DCA4C0, 268, '__createAuxiliaryImageForIndex'),
        )),
    'aux-property': (
        '.local/apple/dsc-20H392/System/Library/Frameworks/MediaToolbox.framework/MediaToolbox',
        '9c23f13c-2605-3ed2-bf6b-f8d537ee6655', (
            (0x195245D68, 224, '_ParseGlobalMetaDataIFFItemPropertyAuxiliaryType'),
            (0x19554AA20, 44, '_IFFInformationGetItemPropertyAuxiliaryType'),
            (0x19554E3F8, 200, '_IFFPictureCopyAuxiliaryType'),
            (0x195305044, 1648, '_PictureReaderCopyProperty'),
        )),
    'aux-tail': (
        '.local/apple/dsc-20H392/System/Library/Frameworks/MediaToolbox.framework/MediaToolbox',
        '9c23f13c-2605-3ed2-bf6b-f8d537ee6655', (
            (0x195245360, 336, '_copyNullTerminatedStringAndAdvanceBuffer'),
            (0x19554A7FC, 224, '_IFFInformationSetItemPropertyAuxiliaryType'),
        )),
    'alpha-writer': (
        '.local/apple/dsc-20H392/System/Library/Frameworks/CoreMedia.framework/CoreMedia',
        '737230ca-7a8d-32b8-91da-d58e680aa9e9', (
            (0x19165E9E0, 548, '_FigHEVCBridge_CreateAlphaInfoSEI'),
        )),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--group', choices=tuple(GROUPS), required=True)
    parser.add_argument('--out', type=Path, required=True, help='new JSON under this checkout x9/out')
    args = parser.parse_args()
    destination = args.out.resolve()
    if not destination.is_relative_to((ROOT / 'x9/out').resolve()) or destination.exists():
        parser.error('--out must be a new file under this checkout x9/out')
    spec = importlib.util.spec_from_file_location('alpha_evidence', Path(__file__).with_name('heif_alpha_evidence.py'))
    evidence = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(evidence)
    image, identity, ranges = GROUPS[args.group]
    evidence.IMAGE = image
    evidence.UUID = identity
    evidence.GROUPS[args.group] = (destination.name, '', ranges)
    result = evidence.replay(args.group, evidence.CACHE)
    result['limit'] = 'fixed new text only; raw auxC origin, not subtype framing or runtime vtable selection'
    with destination.open('x') as output:
        json.dump(result, output, indent=2)
        output.write('\n')
    print(f'qualified {args.group}: {len(ranges)} ranges -> {destination}')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError) as error:
        print(f'error: {error}', file=sys.stderr)
        sys.exit(1)
