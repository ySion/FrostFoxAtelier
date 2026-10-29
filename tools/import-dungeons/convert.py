"""Convert Unity's resolved content to OBJ, v2 material instances and Scene v10."""
import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import uuid
from PIL import Image

NAMESPACE = uuid.UUID('286a5c17-f8ac-4cce-80b8-e3f7817d2a0e')


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def resource_key(path, seed):
    """Existing resource identities survive regeneration; new ones are deterministic."""
    sidecar = Path(str(path) + '.asset.json')
    if sidecar.exists():
        metadata = json.loads(sidecar.read_text(encoding='utf-8'))
        if metadata['version'] != 1:
            raise ValueError('Unsupported resource identity: ' + str(sidecar))
        uuid.UUID(metadata['id'])
    else:
        metadata = dict(version=1, id='{' + str(uuid.uuid5(NAMESPACE, seed)).upper() + '}')
        write_json(sidecar, metadata)
    return 'project/' + metadata['id']


def override(path, kind, value):
    # The assets/material_format.hpp entries schema, not the retired fixed Surface JSON.
    return dict(path=path, type=kind, enabled=True, value=value)


def standard_material(color, texture_key, texture_alpha, roughness):
    coverage = 'mask' if color[3] < 1 or texture_alpha else 'opaque'
    entries = [override('base_color', 'vector4', color), override('roughness', 'float', roughness),
               override('metallic', 'float', 0), override('normal_scale', 'float', 1),
               override('normal_y_sign', 'float', 1), override('emissive', 'vector3', [0, 0, 0]),
               override('alpha_cutoff', 'float', .5)]
    if texture_key:
        entries += [override('base_color_texture', 'texture', dict(asset_key=texture_key, address='repeat')),
                    override('base_color_uv.scale', 'vector2', [1, 1]),
                    override('base_color_uv.offset', 'vector2', [0, 0]),
                    override('base_color_uv.rotation', 'float', 0)]
    return dict(version=2, role='instance', parent_key='builtin/material/standard-' + coverage + '-double', entries=entries)


def component(kind, **values):
    return dict(type=kind, name=kind.rsplit('.', 1)[-1], enabled=True,
                properties={}, **values)


def entity(name, part, identity, position, rotation, scale, active=True):
    part['id'] = str(identity + 1)
    return dict(id=str(identity), type='world.StaticMeshEntity' if part['type']=='world.StaticMeshComponent' else 'world.Entity',
                name=name[-256:], enabled=active,
                space=dict(position=position, rotation=rotation, scale=scale, origin=None),
                children=[], properties={}, components=[part])


def validate_transform(instance):
    x,y,z,w = instance['rotation']
    rotation = ((1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w)),
                (2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w)),
                (2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y)))
    expected = [rotation[row][col]*instance['scale'][col] if col<3 and row<3 else
                instance['position'][row] if col==3 and row<3 else
                1 if col==row==3 else 0 for col in range(4) for row in range(4)]
    if max(abs(a-b) for a,b in zip(expected,instance['matrix'])) > .001:
        raise ValueError('World transform contains unsupported shear: '+instance['path'])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('export', type=Path)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    data = json.loads((args.export / 'export.json').read_text())
    root = args.destination.resolve(); models = root / 'Models'; models.mkdir(parents=True, exist_ok=True)
    textures = models / 'Textures'; textures.mkdir(exist_ok=True)
    materials = {m['id']: m for m in data['materials']}
    material_texture = {}; texture_keys = {}; texture_alpha = {}
    for material in materials.values():
        if not material['texture']:
            continue
        source = root / 'Originals' / material['texture']
        name = source.stem + '_' + hashlib.sha256(material['texture'].encode()).hexdigest()[:10] + '.png'
        target = textures / name
        if not target.exists():
            if source.suffix.lower() == '.png': shutil.copyfile(source, target)
            else:
                with Image.open(source) as image: image.convert('RGBA').save(target)
        material_texture[material['id']] = 'Textures/' + name
        texture_keys[material['id']] = resource_key(target, 'texture|' + material['texture'])
        with Image.open(target) as image:
            texture_alpha[material['id']] = image.convert('RGBA').getchannel('A').getextrema()[0] < 255
    defaults = {}
    for item in data['library']:
        if item['materials'] and any(item['materials']): defaults[item['mesh']] = tuple(item['materials'])
    for scene in reversed(data['scenes']):
        for item in scene['instances']: defaults[item['mesh']] = tuple(item['materials'])
    # Every imported mesh is available in the browser, including unused models and LODs.
    variants = defaultdict(set)
    for mesh in data['meshes']: variants[mesh['id']].add(defaults.get(mesh['id'], ()))
    for scene in data['scenes']:
        for item in scene['instances']: variants[item['mesh']].add(tuple(item['materials']))
    material_keys = {}; material_authors = {}
    for identity in sorted({value for choices in variants.values() for slots in choices for value in slots if value}):
        material = materials[identity]
        color = [max(0, min(1, c)) for c in material['color']]
        author = standard_material(color, texture_keys.get(identity, ''), texture_alpha.get(identity, False), .65)
        suffix = str(uuid.uuid5(NAMESPACE, 'material|' + identity)).upper()[:8]
        name = re.sub(r'[^a-zA-Z0-9_-]', '_', material['name'])[:90] + '_' + suffix
        target = models / 'Materials' / (name + '.material')
        write_json(target, author)
        material_keys[identity] = resource_key(target, 'material|' + identity)
        material_authors[identity] = author
    bindings = {}; mesh_slots = {}; exported = []
    for mesh in data['meshes']:
        raw = (args.export / mesh['file']).read_bytes()
        count, sub_count = struct.unpack_from('<ii', raw); offset = 8
        vertices = list(struct.iter_unpack('<8f', raw[offset:offset+count*32])); offset += count*32
        submeshes = []
        for _ in range(sub_count):
            n, = struct.unpack_from('<i', raw, offset); offset += 4
            submeshes.append(struct.unpack_from('<' + 'i'*n, raw, offset)); offset += n*4
        assert offset == len(raw)
        if not any(submeshes): raise ValueError('Empty mesh: ' + mesh['name'])
        for slots in sorted(variants[mesh['id']]):
            # Authored reflections stay in the instance's negative scale; the mesh is shared.
            body = []
            for v in vertices: body.append('v ' + ' '.join(format(x,'.9g') for x in v[:3]))
            for v in vertices: body.append('vn ' + ' '.join(format(x,'.9g') for x in v[3:6]))
            for v in vertices: body.append('vt ' + ' '.join(format(x,'.9g') for x in v[6:]))
            body = '\n'.join(body) + '\n'
            seed = mesh['id'] + '|' + '|'.join(slots)
            identity = str(uuid.uuid5(NAMESPACE, seed)).upper()
            basename = re.sub(r'[^a-zA-Z0-9_-]', '_', mesh['name'])[:90] + '_' + identity[:8]
            obj = models / (basename + '.obj'); mtl = models / (basename + '.mtl')
            lines = []; mtl_lines = []; slot_materials = {}
            for sub, indices in enumerate(submeshes):
                if not indices: continue
                mat_id = slots[min(sub,len(slots)-1)] if slots else ''
                mat = materials.get(mat_id, {'name':'Unassigned', 'color':[1,1,1,1]})
                name = 'slot_' + str(sub)
                color = [max(0,min(1,c)) for c in mat['color']]
                mtl_lines += ['newmtl '+name, 'Kd '+' '.join(format(c,'.9g') for c in color[:3]), 'd '+format(color[3],'.9g')]
                if mat_id in material_texture: mtl_lines.append('map_Kd '+material_texture[mat_id])
                lines.append('usemtl '+name)
                if mat_id:
                    slot_materials[name] = mat_id
                for i in range(0,len(indices),3):
                    triangle = indices[i:i+3]
                    lines.append('f '+' '.join(f'{v+1}/{v+1}/{v+1}' for v in triangle))
            obj.write_text('mtllib '+mtl.name+'\n'+body+'\n'.join(lines)+'\n',encoding='utf-8')
            mtl.write_text('\n'.join(mtl_lines)+'\n',encoding='utf-8')
            bindings[mesh['id'],slots] = resource_key(obj, seed)
            sidecar = Path(str(obj) + '.asset.json')
            metadata = json.loads(sidecar.read_text(encoding='utf-8'))
            metadata['materials'] = {slot: material_keys[value] for slot, value in slot_materials.items()}
            write_json(sidecar, metadata)
            mesh_slots[mesh['id'], slots] = slot_materials
            exported.append(dict(source=mesh['source'], mesh=mesh['id'], materials=slots, triangles=mesh['triangles'],
                                 path=obj.relative_to(root).as_posix(), resource=bindings[mesh['id'],slots]))
    scenes = []
    for source in data['scenes']:
        entities = []; paths = []; scene_materials = []; visible = 0; next_id = 1
        # Preserve the original entity/component ID sequence; materials occupy
        # their own tail of the same document-local identity space.
        next_material_id = 1 + 2 * (len(source['instances']) + len(source['lights']))
        for instance in source['instances']:
            validate_transform(instance)
            active = instance['active'] and instance['enabled'] and instance['lod'] == 0
            if any(abs(v)<1e-6 for v in instance['scale']):
                raise ValueError('Degenerate authored scale: '+instance['path'])
            mesh_key = (instance['mesh'], tuple(instance['materials']))
            c = component('world.StaticMeshComponent', mesh=bindings[mesh_key], materials=[])
            e = entity(instance['path'], c, next_id, instance['position'],
                       instance['rotation'], instance['scale'], active)
            entities.append(e); next_id += 2; visible += active
            # The former component replaced imported roughness with .8. Preserve
            # that scene-local author intent, including the MTL color/texture, once.
            # Source-unassigned slots have no MI and use DefaultWorldGrid.
            for slot, mat_id in mesh_slots[mesh_key].items():
                base = material_authors[mat_id]
                color = next(entry['value'] for entry in base['entries'] if entry['path'] == 'base_color')
                author = standard_material(color, texture_keys.get(mat_id, ''), texture_alpha.get(mat_id, False), .8)
                identity = str(next_material_id); next_material_id += 1
                scene_materials.append(dict(id=identity, source='', parent=author['parent_key'], entries=author['entries']))
                c['materials'].append(dict(slot=slot, resource=identity, overridden=True))
            paths.append(dict(entity=e['id'], **instance))
        for light in source['lights']:
            if light['type'] not in ('Point','Directional'): raise ValueError('Unsupported light '+light['type'])
            c = component('world.LightComponent')
            c['properties'] = {'settings': dict(kind=light['type'].lower(), color=dict(zip(('x','y','z'),light['color'])),
                intensity=max(0,light['intensity']),range=max(.1,light['range']),source_radius=.05,width=1,height=1,two_sided=False,casts_shadow=True)}
            entities.append(entity(light['path'], c, next_id, light['position'],
                                   light['rotation'], [1, 1, 1], light['enabled']))
            next_id += 2
        file = source['name'].replace(' ','') + '.scene'
        write_json(root/'Scenes'/file,dict(version=10, next_id=str(max(next_id, next_material_id)),
                   roots=[e['id'] for e in entities], entities=entities, materials=scene_materials))
        write_json(root/'Import'/ (source['name'].replace(' ','')+'-instances.json'),paths)
        scenes.append(dict(source=source['source'],file='Scenes/'+file,entities=len(entities),meshes=len(paths),visible_meshes=visible,
                           lights=len(source['lights']),particle_systems_preserved=source['particleSystems'],
                           unity_only_missing_script_components=source['missingScripts'],cameras=source['cameras']))
    write_json(root/'Import/models.json',exported)
    write_json(root/'Import/materials.json',data['materials'])
    write_json(root/'Import/summary.json',dict(unity_version=data['unityVersion'],meshes=len(data['meshes']),
         model_variants=len(exported),fbx_files=len({m['source'] for m in data['meshes'] if m['source'].lower().endswith('.fbx')}),
         materials=len(materials),textures=len(list(textures.glob('*.png'))),scenes=scenes))
    print(json.dumps(dict(meshes=len(data['meshes']),variants=len(exported),scenes=scenes),ensure_ascii=False,indent=2))


if __name__ == '__main__':
    main()
