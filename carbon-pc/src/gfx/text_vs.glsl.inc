R"GLSL(#version 140
    uniform vec4 canvas; 
uniform samplerBuffer glyphTexOffset;
uniform samplerBuffer quads;
out vec2 vsTC;

    void main()
    {
int i = gl_VertexID / 6;
vec4 Position = texelFetch(quads, i*2);
int  Glyph    = int(texelFetch(quads, i*2+1).x);
int id = gl_VertexID % 6;
float x = Position.x;
        float y = Position.y;
        float w = Position.z;
        float h = Position.w;
        vec4 p = vec4(x,y,0,1);
switch(id) {
        case 0: break;
        case 3:
        case 1: p.x += w; break;
        case 5:
        case 2: p.y += h; break;
        case 4: p.x += w; p.y += h; break; 
        }
int g = int(Glyph);
        vec4 ginfo = texelFetch(glyphTexOffset, g);
        x = ginfo.x;
        y = ginfo.y;
        w = ginfo.z;
        h = ginfo.w;
vec2 tc = vec2(x,y);
switch(id) {
        case 0: break;
        case 3:
        case 1: tc.x += w; break;
        case 5:
        case 2: tc.y += h; break;
        case 4: tc.x += w; tc.y += h; break; 
        }
gl_Position = vec4( ((p.x / canvas.x)*canvas.z*2.0 - 1.0), 
                            ((p.y / canvas.y)*2.0 - 1.0), 0, 1.0); 
        vsTC    = tc; 
    }
    )GLSL"
