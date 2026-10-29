module;

#include <common.hxx>

export module screenquad;

// A pretransformed quad over a rectangle of the render target, for the fullscreen passes. The vertices sit half a
// pixel up and left of the pixel edges, where D3D9 puts the texel centres, so UV 0..1 maps texel for pixel.
export struct ScreenVertex
{
    float x, y, z, rhw;
    float u, v;
};

export constexpr DWORD kScreenVertexFVF = D3DFVF_XYZRHW | D3DFVF_TEX1;

export struct ScreenQuad
{
    ScreenVertex vertices[4];

    // A width x height rectangle with its top left at (x, y), in pixels
    static ScreenQuad Rect(float x, float y, float width, float height, float z = 0.0f)
    {
        return { {
            { x - 0.5f,         y - 0.5f,          z, 1.0f, 0.0f, 0.0f },
            { x - 0.5f,         y + height - 0.5f, z, 1.0f, 0.0f, 1.0f },
            { x + width - 0.5f, y - 0.5f,          z, 1.0f, 1.0f, 0.0f },
            { x + width - 0.5f, y + height - 0.5f, z, 1.0f, 1.0f, 1.0f },
        } };
    }

    static ScreenQuad Full(float width, float height)
    {
        return Rect(0.0f, 0.0f, width, height);
    }

    // DrawPrimitiveUP leaves stream 0 unbound: whoever draws from it next needs it put back (SavedVertexInput).
    HRESULT Draw(IDirect3DDevice9* device) const
    {
        return device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(ScreenVertex));
    }
};

// The vertex declaration, the FVF and stream 0, saved on construction and put back on destruction
export class SavedVertexInput
{
public:
    explicit SavedVertexInput(IDirect3DDevice9* device) : device(device)
    {
        device->GetFVF(&fvf);
        device->GetVertexDeclaration(&declaration);
        device->GetStreamSource(0, &stream, &offset, &stride);
    }

    ~SavedVertexInput()
    {
        device->SetFVF(fvf);
        device->SetVertexDeclaration(declaration);
        device->SetStreamSource(0, stream, offset, stride);
        if (declaration)
            declaration->Release();
        if (stream)
            stream->Release();
    }

    SavedVertexInput(const SavedVertexInput&) = delete;
    SavedVertexInput& operator=(const SavedVertexInput&) = delete;

private:
    IDirect3DDevice9* device;
    IDirect3DVertexDeclaration9* declaration = nullptr;
    IDirect3DVertexBuffer9* stream = nullptr;
    UINT offset = 0, stride = 0;
    DWORD fvf = 0;
};
