#version 140
    uniform vec4 color; 
    uniform sampler2D fontTex;
    in vec2 vsTC;
    out vec4 fragColor;

    void main()
    {
        float a = (texture2D( fontTex, vsTC.xy ).x); 
        fragColor.rgb = color.rgb; 
        fragColor.a = color.a * a;
    }
    