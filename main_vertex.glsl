#version 450

layout(location = 0) in vec4 iPosition;
layout(location = 1) in vec2 iTexCoord;

out gl_PerVertex
{
    vec4 gl_Position;
};

layout(location = 0) out vec2 vTexCoord;

layout(set = 0, binding = 1) uniform uUniformBuffer
{
    mat4 uTransform;
};

void main() 
{
    gl_Position = iPosition * uTransform;
    vTexCoord = iTexCoord;
}