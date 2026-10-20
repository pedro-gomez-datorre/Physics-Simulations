#include <GL/glew.h>     // loads OpenGL functions
#include <GLFW/glfw3.h>  // window, input, OpenGL context
#include <cstdio>        // printf for shader error messages
#include <cmath>         // sin, cos, sqrt, exp

// ---------------------------------------------------------------------------
// Vertex shader: just places the 4 corners of a full-screen quad.
// ---------------------------------------------------------------------------
const char* vertSrc = R"(#version 330 core
layout(location = 0) in vec2 pos;
void main() { gl_Position = vec4(pos, 0.0, 1.0); }
)";

// ---------------------------------------------------------------------------
// Fragment shader: runs once per pixel. Each pixel shoots one light ray
// from the camera, bends it around the black hole, and works out its color.
// Units: Schwarzschild radius rs = 1, speed of light c = 1.
// ---------------------------------------------------------------------------
const char* fragSrc = R"(#version 330 core
uniform vec2 resolution;                        // window size in pixels
uniform vec3 camPos, camFwd, camRight, camUp;   // camera position and orientation
uniform float time;                             // seconds since start (for animation)
out vec4 color;                                 // final pixel color

// Cheap pseudo-random number from a 3D point (used to place stars)
float hash(vec3 p) {
    p = fract(p * 0.3183099 + 0.1);
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

// Sparse stars so the bending is visible
// d = direction the ray finally points in
vec3 background(vec3 d) {
    vec3 g = d * 80.0;                           
    float h = hash(floor(g));                    
    float star = step(0.985, h) * smoothstep(0.5, 0.0, length(fract(g) - 0.5));
    
    return vec3(0.0008, 0.0008, 0.002) + star * vec3(1.0, 0.95, 0.85);   
}

float hash21(vec2 p){
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float noise2(vec2 p){
    vec2 i = floor(p);
    vec2 f = fract(p);

    f = f*f*(3.0 - 2.0 *f);

    float a = hash21(i);
    float b = hash21(i + vec2(1.0, 0.0));
    float c = hash21(i + vec2(0.0, 1.0));
    float d = hash21(i + vec2(1.0, 1.0));
    
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

float hash31(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract(p.x * p.y * p.z);
}

float noise3(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    
    float n000 = hash31(i + vec3(0, 0, 0));
    float n100 = hash31(i + vec3(1, 0, 0));
    float n010 = hash31(i + vec3(0, 1, 0));
    float n110 = hash31(i + vec3(1, 1, 0));
    float n001 = hash31(i + vec3(0, 0, 1));
    float n101 = hash31(i + vec3(1, 0, 1));
    float n011 = hash31(i + vec3(0, 1, 1));
    float n111 = hash31(i + vec3(1, 1, 1));

    vec3 u = f * f * (3.0 - 2.0 * f);

    float x00 = mix(n000, n100, u.x);
    float x10 = mix(n010, n110, u.x);
    float x01 = mix(n001, n101, u.x);
    float x11 = mix(n011, n111, u.x);

    float y0 = mix(x00, x10, u.y);
    float y1 = mix(x01, x11, u.y);

    return mix(y0, y1, u.z);
}

vec3 ACESFilm(vec3 x) {
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

float fbm(vec2 p){
    float f = 0.0;
    float amplitude = 0.5;
    
    for (int i = 0; i < 5; i++){
        f += amplitude * noise2(p);
        p = p* 2.0 + vec2(1.0, 1.0);
        amplitude *= 0.5;
    }
    return f;
}

float gasDensity(vec3 p) {
    float r = length(p);
    
    float innerRadius = 3.0;
    float outerRadius = 10.0;

    if (r < innerRadius || r > outerRadius) {
        return 0.0; // Outside the disk region
    }

    float height = 0.025 * r + 0.04;; // Thickness of the disk increases with radius
    
    float verticalDensity = exp(-(p.y * p.y) / (2.0 * height * height)); // Gaussian falloff in y-direction

    float innerFade = smoothstep(innerRadius, innerRadius + 0.5, r);
    float outerFade = 1.0 - smoothstep(outerRadius - 1.0, outerRadius, r);
       
    float radialDensity = pow(3.0 / r, 0.7);
    
    float innerStructure = smoothstep(3.0, 4.0, r);
    radialDensity *= innerStructure;
    
    float largeClouds = noise3(p * 0.35);
    float mediumClouds = noise3(p * 0.8);
    float fineDetail = noise3(p * 1.6);

    float gasNoise = largeClouds * 0.55 + mediumClouds * 0.30 + fineDetail * 0.15;
    radialDensity *= 0.85 + 0.3 * gasNoise; //Add
    
    return radialDensity * verticalDensity * innerFade * outerFade;
}

// Acceleration of a light ray near a Schwarzschild black hole at the origin.
// This is the geodesic equation in Cartesian form: a = -1.5 * h^2 * p / r^5,
// where h = |p x v| is the ray's angular momentum around the hole.
vec3 accel(vec3 p, vec3 v) {
    float r2 = dot(p, p);          // distance squared
    vec3 hv = cross(p, v);         // angular momentum vector
    return -1.5 * dot(hv, hv) * p / (r2 * r2 * sqrt(r2));
}

void main() {
    // Pixel position from -0.5..0.5 vertically, centered on the screen
    vec2 uv = (gl_FragCoord.xy - 0.5 * resolution) / resolution.y;

    // Ray starts at the camera and points through this pixel
    vec3 p = camPos;
    vec3 v = normalize(camFwd + 1.5 * (uv.x * camRight + uv.y * camUp));

    vec3 col = vec3(0.0);       // accumulated color
    float trans = 1.0;          // how much light still passes through (1 = fully clear)
    bool hit = false;           // did the ray fall into the black hole?
    
    float gasTransmittance = 1.0;
    vec3 gasColor = vec3(0.0);

    // Step the ray forward through curved space
    for (int i = 0; i < 600; i++) {
        float r = length(p);                              // distance to the black hole
        if (r < 1.0) { hit = true; break; }               // inside the event horizon: captured
        if (r > 80.0 && dot(p, v) > 0.0) break;           // far away and moving outward: escaped

        float photonRing = exp(-pow(r - 2.6, 2.0) * 8.0);
        col += trans * vec3(1.0, 0.6, 0.2) * photonRing * 0.05;

        vec3 pOld = p;                                    // remember position for the disk test

        // Small steps near the black hole, big steps far away
        float dt = clamp(0.05 * r, 0.02, 1.0);
           
        float density = gasDensity(p);
        
        if (density > 0.001) {
            float temperature = clamp(pow(3.0 / max(r, 3.0), 0.75), 0.0, 1.0); // hotter closer in

            vec3 gasEmmission = mix(vec3(0.25, 0.015, 0.002), vec3(1.0, 0.55, 0.18), temperature); // dark red to hot white
            
            float stepOpacity = 1.0 - exp(-density * dt * 0.6); // scale factor for opacity
            
            gasColor += gasTransmittance * gasEmmission * stepOpacity; // accumulate color
            gasTransmittance *= 1.0 - stepOpacity; // update transmittance
        }

        // RK4: sample the curvature 4 times, then average for an accurate step
        vec3 k1p = v,                 k1v = accel(p, v);
        vec3 k2p = v + 0.5*dt*k1v,    k2v = accel(p + 0.5*dt*k1p, k2p);
        vec3 k3p = v + 0.5*dt*k2v,    k3v = accel(p + 0.5*dt*k2p, k3p);
        vec3 k4p = v + dt*k3v,        k4v = accel(p + dt*k3p, k4p);
        p += dt / 6.0 * (k1p + 2.0*k2p + 2.0*k3p + k4p);                  // new position
        v  = normalize(v + dt / 6.0 * (k1v + 2.0*k2v + 2.0*k3v + k4v));   // new direction (light keeps speed 1)

        // Did this step cross the disk plane (y = 0)?
        if (pOld.y * p.y < 0.0) {
            float f = pOld.y / (pOld.y - p.y);       // how far along the step the crossing happened
            vec3 hp = mix(pOld, p, f);               // exact crossing point
            float hr = length(hp.xz);                // distance from the center, in the disk plane

            // Only the ring between 3 and 10 Schwarzschild radii is glowing matter
            if (hr > 3.0 && hr < 10.0) {
                vec3 orbitDir = cross(vec3(0.0, 1.0, 0.0), hp);   // direction of orbiting matter
                float doppler = clamp(1.0 + 1.2 * dot(orbitDir, -v) * sqrt(0.5 / hr), 0.2, 3.0);   // blue-shift if moving toward us, red-shift if away
                
                float angle = atan(hp.z, hp.x);
                float logR = log(hr);
                
                // Spiral turbulence: multiple scales of rotating structure
                vec2 uvDisk = vec2(angle * 3.0 - time * 0.25 + logR * 3.0, logR * 9.0 - time * 0.5);

                float clouds = fbm(uvDisk);
                float detail = fbm(uvDisk * 2.7 + vec2(4.2, 8.1));
                
                // Bright turbulent filaments and dark gaps
                float turbulence = clamp(0.25 + clouds * 1.5 + (detail - 0.5) * 0.9, 0.05, 1.5);

                float temperature = clamp(pow(3.0 / hr, 0.75), 0.0, 1.0); //hotter closer in
                
                //------------------------------------------------------------
                float effectiveTemp = clamp(temperature * doppler, 0.0, 1.0);

                vec3 darkRed = vec3(0.16, 0.012, 0.003);
                vec3 orange = vec3(1.0, 0.12, 0.015);
                vec3 yellow = vec3(1.0, 0.48, 0.12);
                vec3 hotWhite = vec3(1.0, 0.62, 0.32); // less white


                vec3 diskColor = mix(darkRed, orange, smoothstep(0.0, 0.4, effectiveTemp));
                diskColor = mix(diskColor, yellow, smoothstep(0.3, 0.7, effectiveTemp));
                diskColor = mix(diskColor, hotWhite, smoothstep(0.65, 1.0, effectiveTemp));

                diskColor *= turbulence;

                // Relativistic brightness asymmetry
                float dopplerBrightness = mix(0.75, 1.3, (doppler - 0.2) / 2.8);
                diskColor *= dopplerBrightness;
                
                //-----------------------------------------------------------

                // Fade the inner and outer edges smoothly
                float innerEdge = smoothstep(3.0, 3.5, hr);
                float outerEdge = 1.0 - smoothstep(6.4, 7.0, hr);
                float edgeFade = innerEdge * outerEdge;

                // Radial brightness falloff
                float brightness = 0.8 * pow(3.0 / hr, 0.8);


                diskColor *= brightness * edgeFade;

                col += trans * diskColor;

                // The disk is optically thick in this approximation
                trans *= 0.15;
            }
        }
    }

    // If the ray escaped, add the starry sky behind it (the lensed view)
    col += gasColor;

    // Si el rayo escapó, añadimos el fondo estrellado
    if (!hit) {
        col += gasTransmittance * background(normalize(v));
    }

    // Aplicamos Tone Mapping y Corrección Gamma
    col = ACESFilm(col);
    col = pow(col, vec3(1.0 / 2.2));

    color = vec4(col, 1.0);
}
)";

// Compile one shader and print any error message to the console
GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[1024]; glGetShaderInfoLog(s, 1024, nullptr, log); printf("%s\n", log); }
    return s;
}

// Orbit camera state: angle around, angle up, and distance from the black hole
float camAz = 0.6f, camEl = 0.3f, camDist = 20.0f;

// Mouse wheel zooms in and out
void scrollCb(GLFWwindow* w, double, double dy) { camDist *= (float)std::exp(-dy * 0.1); }

// Make a 3D vector length 1
void normalize3(float v[3]) {
    float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    v[0] /= l; v[1] /= l; v[2] /= l;
}

// Cross product a x b, stored in out
void cross3(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

int main() {
    // --- Window and OpenGL setup ---
    if (!glfwInit()) return -1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* w = glfwCreateWindow(800, 600, "BlackHole3D", nullptr, nullptr);
    if (!w) { glfwTerminate(); return -1; }
    glfwMakeContextCurrent(w);
    glfwSetScrollCallback(w, scrollCb);                          // register mouse wheel handler
    double lastX = 0, lastY = 0;                                 // previous mouse position
    glewExperimental = GL_TRUE;
    glewInit();

    // --- Build the shader program ---
    GLuint prog = glCreateProgram();
    glAttachShader(prog, compile(GL_VERTEX_SHADER, vertSrc));
    glAttachShader(prog, compile(GL_FRAGMENT_SHADER, fragSrc));
    glLinkProgram(prog);

    // --- Full-screen quad (two triangles) that the shader paints on ---
    float quad[] = { -1,-1, 1,-1, -1,1, 1,1 };
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);                                  // vertex array: remembers the layout below
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);                                       // buffer: holds the corner points on the GPU
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr); // 2 floats per vertex

    // --- Main loop: runs once per frame ---
    while (!glfwWindowShouldClose(w)) {
        int fw, fh; glfwGetFramebufferSize(w, &fw, &fh);         // current window size in pixels
        glViewport(0, 0, fw, fh);

        // Mouse drag orbits the camera
        double mx, my; glfwGetCursorPos(w, &mx, &my);
        if (glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
            camAz -= (float)(mx - lastX) * 0.005f;               // left/right
            camEl += (float)(my - lastY) * 0.005f;               // up/down
            if (camEl > 1.5f) camEl = 1.5f;                      // do not flip over the top
            if (camEl < -1.5f) camEl = -1.5f;
        }
        lastX = mx; lastY = my;

        // Camera position on a sphere around the black hole, and its look direction
        float pos[3] = { camDist * std::cos(camEl) * std::sin(camAz), camDist * std::sin(camEl), camDist * std::cos(camEl) * std::cos(camAz) };
        float fwd[3] = { -pos[0], -pos[1], -pos[2] }; normalize3(fwd);   // always look at the center
        float up0[3] = { 0, 1, 0 };                                       // world "up"
        float right[3]; cross3(fwd, up0, right); normalize3(right);       // camera right
        float up[3]; cross3(right, fwd, up); normalize3(up);              // camera up

        // Send everything to the shader
        glUseProgram(prog);
        glUniform3fv(glGetUniformLocation(prog, "camPos"), 1, pos);
        glUniform3fv(glGetUniformLocation(prog, "camFwd"), 1, fwd);
        glUniform3fv(glGetUniformLocation(prog, "camRight"), 1, right);
        glUniform3fv(glGetUniformLocation(prog, "camUp"), 1, up);
        glUniform2f(glGetUniformLocation(prog, "resolution"), (float)fw, (float)fh);
        glUniform1f(glGetUniformLocation(prog, "time"), (float)glfwGetTime());

        // Draw the quad: the fragment shader runs for every pixel
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        glfwSwapBuffers(w);
        glfwPollEvents();
        if (glfwGetKey(w, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
    }
    glfwTerminate();
}
