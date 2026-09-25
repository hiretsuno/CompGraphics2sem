"""Конвертирует Cerberus (glTF 1.0, javagl/gltfTestModels mirror оригинального ассета
Andrew Maximov, 2014, non-commercial/educational use) в OBJ+MTL, который понимает
загрузчик проекта (RenderingSystem::LoadObj / LoadMtlData).

Почему не грузим .gltf напрямую: движок читает только Wavefront OBJ. glTF здесь -
только промежуточный формат, в котором был доступен этот конкретный ассет
(оригинальный .obj с сайта автора недоступен).

Один mesh, один материал, атрибуты не interleaved (у каждого свой bufferView),
индексы uint16 - парсится в лоб через struct, без внешних библиотек.

Запуск из корня репозитория:  python tools/gltf_to_obj_cerberus.py <src_dir> <dst_dir>
    src_dir - папка с Cerberus.gltf/.bin/*.jpg (скачанные файлы)
    dst_dir - models/cerberus (создаётся)
"""
import json
import os
import shutil
import struct
import sys

COMPONENT_SIZES = {5120: 1, 5121: 1, 5122: 2, 5123: 2, 5125: 4, 5126: 4}
COMPONENT_FMT = {5123: 'H', 5126: 'f'}
TYPE_COUNTS = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}


def read_accessor(gltf, buffer_bytes, name):
    acc = gltf['accessors'][name]
    view = gltf['bufferViews'][acc['bufferView']]
    count = acc['count']
    n = TYPE_COUNTS[acc['type']]
    fmt = '<' + COMPONENT_FMT[acc['componentType']] * n
    size = COMPONENT_SIZES[acc['componentType']] * n
    base = view['byteOffset'] + acc.get('byteOffset', 0)
    out = []
    for i in range(count):
        off = base + i * size
        out.append(struct.unpack_from(fmt, buffer_bytes, off))
    return out


def main():
    src_dir, dst_dir = sys.argv[1], sys.argv[2]
    gltf = json.load(open(os.path.join(src_dir, 'Cerberus.gltf'), encoding='utf-8'))
    buf = open(os.path.join(src_dir, 'Cerberus.bin'), 'rb').read()

    prim = gltf['meshes']['mesh0']['primitives'][0]
    positions = read_accessor(gltf, buf, prim['attributes']['POSITION'])
    normals = read_accessor(gltf, buf, prim['attributes']['NORMAL'])
    uvs = read_accessor(gltf, buf, prim['attributes']['TEXCOORD_0'])
    indices = [v[0] for v in read_accessor(gltf, buf, prim['indices'])]
    assert len(positions) == len(normals) == len(uvs)
    assert len(indices) % 3 == 0

    os.makedirs(os.path.join(dst_dir, 'textures'), exist_ok=True)
    for f in ('Cerberus_A.jpg', 'Cerberus_N.jpg', 'Cerberus_MR.jpg'):
        shutil.copyfile(os.path.join(src_dir, f), os.path.join(dst_dir, 'textures', f))

    obj_path = os.path.join(dst_dir, 'cerberus.obj')
    with open(obj_path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# Cerberus (Andrew Maximov, 2014) - converted from glTF via tools/gltf_to_obj_cerberus.py\n')
        f.write('mtllib cerberus.mtl\n')
        for p in positions:
            f.write('v %.6f %.6f %.6f\n' % p)
        for uv in uvs:
            # LoadObj делает uv.y = 1 - uv.y при чтении -> пишем уже перевёрнутым,
            # чтобы после чтения получить исходный V из glTF (там v=0 - верх картинки,
            # ровно как и в текстуре после WIC-загрузки без переворота).
            f.write('vt %.6f %.6f\n' % (uv[0], 1.0 - uv[1]))
        for n in normals:
            f.write('vn %.6f %.6f %.6f\n' % n)
        f.write('usemtl cerberus\n')
        for i in range(0, len(indices), 3):
            a, b, c = indices[i] + 1, indices[i + 1] + 1, indices[i + 2] + 1
            f.write('f %d/%d/%d %d/%d/%d %d/%d/%d\n' % (a, a, a, b, b, b, c, c, c))

    mat = gltf['materials']['pbrMaterial']['values']
    mtl_path = os.path.join(dst_dir, 'cerberus.mtl')
    with open(mtl_path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# Cerberus material - metallic/roughness из фактора * канал текстуры (см. map_MR в RenderingSystem.cpp)\n')
        f.write('newmtl cerberus\n')
        f.write('Kd 1.000000 1.000000 1.000000\n')
        f.write('Ns 32\n')
        f.write('Pm %.4f\n' % mat['metallicFactor'][0])
        f.write('Pr %.4f\n' % mat['roughnessFactor'][0])
        f.write('map_Kd textures/Cerberus_A.jpg\n')
        f.write('map_Bump textures/Cerberus_N.jpg\n')
        f.write('map_MR textures/Cerberus_MR.jpg\n')

    print('vertices:', len(positions), 'triangles:', len(indices) // 3)
    print('wrote', obj_path)
    print('wrote', mtl_path)


if __name__ == '__main__':
    main()
