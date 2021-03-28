#version 450

layout(location = 0) in vec4 iPosition;
layout(location = 1) in vec4 iColor;

out gl_PerVertex
{
    vec4 gl_Position;
};

layout(location = 0) out vec4 vColor;

void main() 
{
    gl_Position = iPosition;
    vColor = iColor;
}