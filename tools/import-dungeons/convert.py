"""Convert Unity's resolved meshes/materials/scenes to the existing OBJ/Scene formats."""
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


def component(kind, **values):
    return dict(id=1, type=kind, enabled=True, parent=0, position=[0,0,0],
                rotation=[0,0,0,1], scale=[1,1,1], properties={}, **values)


def actor(name, part, active=True):
    return dict(type='world.StaticMeshActor' if part['type']=='world.StaticMeshComponent' else 'world.Actor',
                name=name[-256:], enabled=active, root=1, properties={}, components=[part])


def scale_sign(instance):
    return tuple(-1 if x < 0 else 1 for x in instance['scale'])


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
    material_texture = {}
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
    defaults = {}
    for item in data['library']:
        if item['materials'] and any(item['materials']): defaults[item['mesh']] = tuple(item['materials'])
    for scene in reversed(data['scenes']):
        for item in scene['instances']: defaults[item['mesh']] = tuple(item['materials'])
    # Every imported mesh is available in the browser, including unused models and LODs.
    variants = defaultdict(set)
    for mesh in data['meshes']: variants[mesh['id']].add((defaults.get(mesh['id'], ()), (1,1,1)))
    for scene in data['scenes']:
        for item in scene['instances']: variants[item['mesh']].add((tuple(item['materials']), scale_sign(item)))
    bindings = {}; exported = []
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
        for slots, signs in sorted(variants[mesh['id']]):
            # Runtime TRS requires positive scale. Preserve authored reflections in
            # mesh positions/normals and reverse winding for odd reflections.
            body = []
            for v in vertices: body.append('v ' + ' '.join(format(x*s,'.9g') for x,s in zip(v[:3],signs)))
            for v in vertices: body.append('vn ' + ' '.join(format(x*s,'.9g') for x,s in zip(v[3:6],signs)))
            for v in vertices: body.append('vt ' + ' '.join(format(x,'.9g') for x in v[6:]))
            body = '\n'.join(body) + '\n'
            reflected = signs != (1,1,1)
            seed = mesh['id'] + '|' + '|'.join(slots)
            if reflected: seed += '|reflection:' + ','.join(map(str,signs))
            identity = str(uuid.uuid5(NAMESPACE, seed)).upper()
            basename = re.sub(r'[^a-zA-Z0-9_-]', '_', mesh['name'])[:90] + '_' + identity[:8]
            obj = models / (basename + '.obj'); mtl = models / (basename + '.mtl')
            lines = []; mtl_lines = []
            for sub, indices in enumerate(submeshes):
                if not indices: continue
                mat_id = slots[min(sub,len(slots)-1)] if slots else ''
                mat = materials.get(mat_id, {'name':'Unassigned', 'color':[1,1,1,1]})
                name = 'slot_' + str(sub)
                color = [max(0,min(1,c)) for c in mat['color']]
                mtl_lines += ['newmtl '+name, 'Kd '+' '.join(format(c,'.9g') for c in color[:3]), 'd '+format(color[3],'.9g')]
                if mat_id in material_texture: mtl_lines.append('map_Kd '+material_texture[mat_id])
                lines.append('usemtl '+name)
                for i in range(0,len(indices),3):
                    triangle = indices[i:i+3]
                    if signs[0]*signs[1]*signs[2]<0: triangle = triangle[::-1]
                    lines.append('f '+' '.join(f'{v+1}/{v+1}/{v+1}' for v in triangle))
            obj.write_text('mtllib '+mtl.name+'\n'+body+'\n'.join(lines)+'\n',encoding='utf-8')
            mtl.write_text('\n'.join(mtl_lines)+'\n',encoding='utf-8')
            write_json(Path(str(obj)+'.asset.json'),dict(version=1,id='{'+identity+'}'))
            bindings[mesh['id'],slots,signs] = 'project/{'+identity+'}'
            exported.append(dict(source=mesh['source'], mesh=mesh['id'], materials=slots, triangles=mesh['triangles'],
                                 reflection=signs, path=obj.relative_to(root).as_posix(), resource=bindings[mesh['id'],slots,signs]))
    scenes = []
    for source in data['scenes']:
        actors = []; paths = []; visible = 0
        for instance in source['instances']:
            validate_transform(instance)
            active = instance['active'] and instance['enabled'] and instance['lod'] == 0
            if any(abs(v)<1e-6 for v in instance['scale']):
                raise ValueError('Degenerate authored scale: '+instance['path'])
            c = component('world.StaticMeshComponent',mesh=bindings[instance['mesh'],tuple(instance['materials']),scale_sign(instance)],
                          base_color=[1,1,1],roughness=.8,metallic=0,emissive=[0,0,0])
            c.update(position=instance['position'],rotation=instance['rotation'],scale=[abs(x) for x in instance['scale']])
            actors.append(actor(instance['path'], c, active)); visible += active
            paths.append(dict(actor=len(actors), **instance))
        for light in source['lights']:
            if light['type'] not in ('Point','Directional'): raise ValueError('Unsupported light '+light['type'])
            c = component('world.LightComponent')
            c.update(position=light['position'],rotation=light['rotation'])
            c['properties'] = {'settings': dict(kind=light['type'].lower(), color=dict(zip(('x','y','z'),light['color'])),
                intensity=max(0,light['intensity']),range=max(.1,light['range']),source_radius=.05,width=1,height=1,two_sided=False,casts_shadow=True)}
            actors.append(actor(light['path'],c,light['enabled']))
        file = source['name'].replace(' ','') + '.scene'
        write_json(root/'Scenes'/file,dict(version=2,actors=actors))
        write_json(root/'Import'/ (source['name'].replace(' ','')+'-instances.json'),paths)
        scenes.append(dict(source=source['source'],file='Scenes/'+file,actors=len(actors),meshes=len(paths),visible_meshes=visible,
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
