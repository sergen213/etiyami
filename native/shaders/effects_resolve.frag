#version 460
layout(location=0) flat in vec4 gain;
layout(location=1) flat in vec4 reflection;
layout(location=0) out vec4 gainOut;
layout(location=1) out vec4 reflectionOut;
void main() { gainOut=gain; reflectionOut=reflection; }
