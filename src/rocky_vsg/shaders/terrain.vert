#version 450

#if 0
#include "terrain.sdk"
#else
layout(set = 0, binding = 10) uniform sampler2D elevation_tex;

layout(set = 0, binding = 13) uniform TileData {
    mat4 elevation_matrix;
    mat4 color_matrix;
    mat4 normal_matrix;
    vec2 elevTexelCoeff;
} tile;
#endif

layout(push_constant) uniform PushConstants
{
    mat4 projection;
    mat4 modelview;
} pc;

layout(location = 0) in vec3 inVertex;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inUV;
layout(location = 3) in vec3 inNeighborVertex;
layout(location = 4) in vec3 inNeighborNormal;

layout(location = 0) out vec3 frag_color;
layout(location = 1) out vec2 frag_uv;
layout(location = 2) out vec3 oe_UpVectorView;
layout(location = 3) out vec2 oe_normalMapCoords;
layout(location = 4) out vec3 oe_normalMapBinormal;

out gl_PerVertex {
    vec4 gl_Position;
};

const float elev_tile_size = 257;
const float elev_tile_bias = 0.5;
const vec2 elev_tile_coeff = vec2(
    (elev_tile_size - (2.0*elev_tile_bias)) / elev_tile_size,
    elev_tile_bias / elev_tile_size);

// Sample the elevation data at a UV tile coordinate
float terrain_get_elevation(in vec2 uv)
{
    // Texel-level scale and bias allow us to sample the elevation texture
    // on texel center instead of edge.
    vec2 elevc = uv
        * elev_tile_coeff.x * tile.elevation_matrix[0][0] // scale
        + elev_tile_coeff.x * tile.elevation_matrix[3].st // bias
        + elev_tile_coeff.y;

    return texture(elevation_tex, elevc).r;
}

void main()
{
    float elevation = terrain_get_elevation(inUV.st);

    vec3 position = inVertex + inNormal*elevation;
    gl_Position = (pc.projection * pc.modelview) * vec4(position, 1.0);

    mat3 normalMatrix = mat3(transpose(inverse(pc.modelview)));
    
    frag_color = vec3(1);
    frag_uv = (tile.color_matrix * vec4(inUV.st, 0, 1)).st;

    // The normal map stuff
    oe_UpVectorView = normalMatrix * inNormal;
    oe_normalMapCoords = inUV.st;
    oe_normalMapBinormal = normalize(normalMatrix * vec3(0.0, 1.0, 0.0));
}
