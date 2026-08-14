#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <d3dcompiler.h>
#include <vector>
#include <thread>
#include "resource.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "winmm.lib")

#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAY_EXIT 1001

HWND g_hWndOverlay = NULL;
HWND g_hWndDummy = NULL;
ID3D11Device* g_pd3dDevice = NULL;
ID3D11DeviceContext* g_pImmediateContext = NULL;
bool g_isAnimating = false;
NOTIFYICONDATA g_nid = { 0 };
HICON g_hCustomIcon = NULL;
HHOOK g_hMouseHook = NULL;

ID3D11VertexShader* g_pGlobalVS = NULL;
ID3D11PixelShader* g_pGlobalPS = NULL;

struct AnimationParams {
    float Time; float Center[2]; float AspectRatio; BOOL IsClosing; float padding[3];
};

HBITMAP CaptureWindow(HWND hWnd) {
    if (!IsWindow(hWnd)) return NULL;
    RECT rc; GetWindowRect(hWnd, &rc);
    int width = rc.right - rc.left, height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) return NULL;

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hBitmap = CreateCompatibleBitmap(hdcScreen, width, height);
    HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, hBitmap);

    BOOL bCaptured = PrintWindow(hWnd, hdcMem, PW_RENDERFULLCONTENT | PW_CLIENTONLY);
    if (!bCaptured) {
        HDC hdcTarget = GetWindowDC(hWnd);
        BitBlt(hdcMem, 0, 0, width, height, hdcTarget, 0, 0, SRCCOPY);
        ReleaseDC(hWnd, hdcTarget);
    }

    SelectObject(hdcMem, hOldBitmap);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
    return hBitmap;
}

void CreateOverlayWindow() {
    WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_CLASSDC, DefWindowProc, 0L, 0L, GetModuleHandle(NULL), NULL, NULL, NULL, NULL, L"KamuiOverlay", NULL };
    RegisterClassEx(&wc);
    g_hWndOverlay = CreateWindowEx(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW, L"KamuiOverlay", L"Kamui Effect", WS_POPUP, 0, 0, 1, 1, NULL, NULL, wc.hInstance, NULL);
    SetLayeredWindowAttributes(g_hWndOverlay, 0, 255, LWA_ALPHA);
}

// 直接从内置资源中编译 HLSL 着色器
bool InitD3DDevice() {
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &g_pd3dDevice, NULL, &g_pImmediateContext))) {
        return false;
    }

    HRSRC hRes = FindResource(NULL, MAKEINTRESOURCE(IDR_KAMUI_HLSL), RT_RCDATA);
    if (!hRes) return false;
    HGLOBAL hData = LoadResource(NULL, hRes);
    if (!hData) return false;
    LPVOID pShaderData = LockResource(hData);
    DWORD shaderSize = SizeofResource(NULL, hRes);
    if (!pShaderData || shaderSize == 0) return false;

    ID3DBlob* vsBlob = NULL;
    ID3DBlob* psBlob = NULL;

    if (SUCCEEDED(D3DCompile(pShaderData, shaderSize, "Kamui.hlsl", NULL, NULL, "VS", "vs_5_0", 0, 0, &vsBlob, NULL))) {
        g_pd3dDevice->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), NULL, &g_pGlobalVS);
        vsBlob->Release();
    }
    if (SUCCEEDED(D3DCompile(pShaderData, shaderSize, "Kamui.hlsl", NULL, NULL, "PS", "ps_5_0", 0, 0, &psBlob, NULL))) {
        g_pd3dDevice->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), NULL, &g_pGlobalPS);
        psBlob->Release();
    }

    return (g_pGlobalVS != NULL && g_pGlobalPS != NULL);
}

void PlayKamuiAnimation(HWND targetHwnd, bool isClosing) {
    if (g_isAnimating || !IsWindow(targetHwnd)) return;
    g_isAnimating = true;

    HBITMAP hBmp = CaptureWindow(targetHwnd);
    if (isClosing) {
        ShowWindow(targetHwnd, SW_HIDE);
    }

    if (!hBmp) {
        if (isClosing) PostMessage(targetHwnd, WM_CLOSE, 0, 0);
        else ShowWindow(targetHwnd, SW_SHOW);
        g_isAnimating = false;
        return;
    }

    RECT rc; GetWindowRect(targetHwnd, &rc);
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) {
        DeleteObject(hBmp);
        if (isClosing) PostMessage(targetHwnd, WM_CLOSE, 0, 0);
        else ShowWindow(targetHwnd, SW_SHOW);
        g_isAnimating = false;
        return;
    }

    if (!isClosing) {
        ShowWindow(targetHwnd, SW_SHOW); // 预留
    }

    SetWindowPos(g_hWndOverlay, HWND_TOPMOST, rc.left, rc.top, width, height, SWP_SHOWWINDOW);
    MARGINS margin = { -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(g_hWndOverlay, &margin);

    IDXGIDevice* pDXGIDevice = NULL; g_pd3dDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&pDXGIDevice);
    IDXGIAdapter* pAdapter = NULL; if (pDXGIDevice) pDXGIDevice->GetAdapter(&pAdapter);
    IDXGIFactory* pFactory = NULL; if (pAdapter) pAdapter->GetParent(__uuidof(IDXGIFactory), (void**)&pFactory);

    DXGI_SWAP_CHAIN_DESC sd = { 0 };
    sd.BufferDesc.Width = width; sd.BufferDesc.Height = height;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 1; sd.OutputWindow = g_hWndOverlay; sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* pSwapChain = NULL;
    if (pFactory) {
        pFactory->CreateSwapChain(g_pd3dDevice, &sd, &pSwapChain);
        pFactory->Release();
    }
    if (pAdapter) pAdapter->Release();
    if (pDXGIDevice) pDXGIDevice->Release();

    if (!pSwapChain) {
        DeleteObject(hBmp);
        if (isClosing) PostMessage(targetHwnd, WM_CLOSE, 0, 0);
        else ShowWindow(targetHwnd, SW_SHOW);
        g_isAnimating = false;
        ShowWindow(g_hWndOverlay, SW_HIDE);
        return;
    }

    ID3D11Texture2D* pBackBuffer = NULL;
    pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBackBuffer);
    ID3D11RenderTargetView* pRTV = NULL;
    if (pBackBuffer) {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, NULL, &pRTV);
        pBackBuffer->Release();
    }

    BITMAP bmp; GetObject(hBmp, sizeof(BITMAP), &bmp);
    BITMAPINFOHEADER bi = { sizeof(BITMAPINFOHEADER), bmp.bmWidth, -bmp.bmHeight, 1, 32, BI_RGB };
    std::vector<BYTE> pixels(bmp.bmWidth * bmp.bmHeight * 4);
    HDC hdc = GetDC(NULL); GetDIBits(hdc, hBmp, 0, bmp.bmHeight, pixels.data(), (BITMAPINFO*)&bi, DIB_RGB_COLORS); ReleaseDC(NULL, hdc);
    for (size_t i = 0; i < pixels.size(); i += 4) { std::swap(pixels[i], pixels[i + 2]); }

    D3D11_TEXTURE2D_DESC texDesc = { 0 };
    texDesc.Width = bmp.bmWidth; texDesc.Height = bmp.bmHeight;
    texDesc.MipLevels = 1; texDesc.ArraySize = 1; texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1; texDesc.Usage = D3D11_USAGE_DEFAULT; texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initData = { pixels.data(), (UINT)(bmp.bmWidth * 4), 0 };

    ID3D11Texture2D* pTexture = NULL; g_pd3dDevice->CreateTexture2D(&texDesc, &initData, &pTexture);
    ID3D11ShaderResourceView* pSRV = NULL;
    if (pTexture) {
        g_pd3dDevice->CreateShaderResourceView(pTexture, NULL, &pSRV);
        pTexture->Release();
    }

    D3D11_BUFFER_DESC cbDesc = { sizeof(AnimationParams), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
    ID3D11Buffer* pConstantBuffer = NULL; g_pd3dDevice->CreateBuffer(&cbDesc, NULL, &pConstantBuffer);
    D3D11_SAMPLER_DESC sampDesc = { D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_BORDER, D3D11_TEXTURE_ADDRESS_BORDER, D3D11_TEXTURE_ADDRESS_BORDER };
    ID3D11SamplerState* pSampler = NULL; g_pd3dDevice->CreateSamplerState(&sampDesc, &pSampler);

    g_pImmediateContext->OMSetRenderTargets(1, &pRTV, NULL);
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
    g_pImmediateContext->RSSetViewports(1, &vp);
    g_pImmediateContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    if (g_pGlobalVS) g_pImmediateContext->VSSetShader(g_pGlobalVS, NULL, 0);
    if (g_pGlobalPS) g_pImmediateContext->PSSetShader(g_pGlobalPS, NULL, 0);
    if (pSRV) g_pImmediateContext->PSSetShaderResources(0, 1, &pSRV);
    if (pSampler) g_pImmediateContext->PSSetSamplers(0, 1, &pSampler);

    ULONGLONG startTime = GetTickCount64();
    float duration = 0.5f;
    while (true) {
        float timeElapsed = (float)(GetTickCount64() - startTime) / 1000.0f;
        if (timeElapsed > duration) break;

        float progress = timeElapsed / duration;
        if (!isClosing) progress = 1.0f - progress;

        AnimationParams params = { progress, {0.5f, 0.5f}, (float)width / height, isClosing ? TRUE : FALSE };
        if (pConstantBuffer) {
            D3D11_MAPPED_SUBRESOURCE mappedResource;
            if (SUCCEEDED(g_pImmediateContext->Map(pConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource))) {
                if (mappedResource.pData) memcpy(mappedResource.pData, &params, sizeof(AnimationParams));
                g_pImmediateContext->Unmap(pConstantBuffer, 0);
            }
            g_pImmediateContext->PSSetConstantBuffers(0, 1, &pConstantBuffer);
        }

        float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        if (pRTV) g_pImmediateContext->ClearRenderTargetView(pRTV, clearColor);
        g_pImmediateContext->Draw(3, 0);
        pSwapChain->Present(1, 0);
    }

    ShowWindow(g_hWndOverlay, SW_HIDE);

    if (isClosing) {
        PostMessage(targetHwnd, WM_CLOSE, 0, 0);
    }
    else {
        ShowWindow(targetHwnd, SW_SHOW);
    }

    if (pSRV) pSRV->Release();
    if (pConstantBuffer) pConstantBuffer->Release(); if (pSampler) pSampler->Release();
    if (pRTV) pRTV->Release(); if (pSwapChain) pSwapChain->Release();
    DeleteObject(hBmp);
    g_isAnimating = false;
}

LRESULT CALLBACK MouseHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && wParam == WM_LBUTTONDOWN) {
        MSLLHOOKSTRUCT* pMouse = (MSLLHOOKSTRUCT*)lParam;
        POINT pt = pMouse->pt;

        HWND hWnd = WindowFromPoint(pt);
        if (hWnd && !g_isAnimating) {
            HWND hRoot = GetAncestor(hWnd, GA_ROOT);
            if (hRoot && IsWindowVisible(hRoot)) {

                RECT rcWindow, rcScreen;
                GetWindowRect(hRoot, &rcWindow);
                rcScreen.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
                rcScreen.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
                rcScreen.right = rcScreen.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
                rcScreen.bottom = rcScreen.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);

                if (rcWindow.left <= rcScreen.left && rcWindow.top <= rcScreen.top &&
                    rcWindow.right >= rcScreen.right && rcWindow.bottom >= rcScreen.bottom) {
                    return CallNextHookEx(g_hMouseHook, nCode, wParam, lParam);
                }

                wchar_t className[256];
                GetClassName(hRoot, className, 256);
                if (wcscmp(className, L"Shell_TrayWnd") == 0 || wcscmp(className, L"Progman") == 0 || wcscmp(className, L"WorkerW") == 0) {
                    return CallNextHookEx(g_hMouseHook, nCode, wParam, lParam);
                }

                LONG_PTR style = GetWindowLongPtr(hRoot, GWL_STYLE);
                if (!(style & WS_CAPTION) || !(style & WS_SYSMENU)) {
                    return CallNextHookEx(g_hMouseHook, nCode, wParam, lParam);
                }

                int closeBtnWidth = 65;
                int closeBtnHeight = 40;

                if (pt.x >= (rcWindow.right - closeBtnWidth) && pt.x <= rcWindow.right &&
                    pt.y >= rcWindow.top && pt.y <= (rcWindow.top + closeBtnHeight)) {

                    // 【零延迟内嵌音频播放】：直接从资源内存地址并发播放音效
                    HRSRC hRes = FindResource(NULL, MAKEINTRESOURCE(IDR_KAMUI_WAV), RT_RCDATA);
                    if (hRes) {
                        HGLOBAL hData = LoadResource(NULL, hRes);
                        if (hData) {
                            LPCSTR pWaveData = (LPCSTR)LockResource(hData);
                            if (pWaveData) {
                                PlaySoundA(pWaveData, NULL, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
                            }
                        }
                    }

                    std::thread(PlayKamuiAnimation, hRoot, true).detach();
                    return 1;
                }
            }
        }
    }
    return CallNextHookEx(g_hMouseHook, nCode, wParam, lParam);
}

LRESULT CALLBACK DummyWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TRAYICON:
        if (lp == WM_RBUTTONUP) {
            POINT pt; GetCursorPos(&pt);
            SetForegroundWindow(hwnd);
            HMENU hMenu = CreatePopupMenu();
            AppendMenu(hMenu, MF_STRING, ID_TRAY_EXIT, L"退出神威引擎");
            TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_RIGHTALIGN, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(hMenu);
        }
        break;

    case WM_COMMAND:
        if (LOWORD(wp) == ID_TRAY_EXIT) {
            Shell_NotifyIcon(NIM_DELETE, &g_nid);
            PostQuitMessage(0);
        }
        break;

    case WM_DESTROY:
        Shell_NotifyIcon(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hwnd, msg, wp, lp);
    }
    return 0;
}

int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR lpCmdLine, _In_ int nCmdShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // 直接从资源内嵌数据加载托盘图标
    g_hCustomIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_KAMUI_ICON));
    if (!g_hCustomIcon) g_hCustomIcon = LoadIcon(NULL, IDI_APPLICATION);

    WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_CLASSDC, DummyWndProc, 0L, 0L, hInstance, NULL, g_hCustomIcon, NULL, NULL, L"KamuiDummyClass", NULL };
    RegisterClassEx(&wc);
    g_hWndDummy = CreateWindowEx(0, L"KamuiDummyClass", L"KamuiTrayHost", WS_OVERLAPPEDWINDOW, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);

    CreateOverlayWindow();
    if (!InitD3DDevice()) {
        MessageBoxW(NULL, L"D3D 初始化或内嵌着色器加载失败！", L"神威引擎错误", MB_OK | MB_ICONERROR);
        return -1;
    }

    g_hMouseHook = SetWindowsHookEx(WH_MOUSE_LL, MouseHookProc, hInstance, 0);
    if (!g_hMouseHook) {
        MessageBoxW(NULL, L"全局鼠标钩子安装失败！", L"神威引擎错误", MB_OK | MB_ICONERROR);
        return -1;
    }

    g_nid.cbSize = sizeof(NOTIFYICONDATA);
    g_nid.hWnd = g_hWndDummy;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = g_hCustomIcon;
    wcscpy_s(g_nid.szTip, L"神威空间引擎 (全内嵌单文件版)");
    Shell_NotifyIcon(NIM_ADD, &g_nid);

    MSG msg = { 0 };
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_hMouseHook) UnhookWindowsHookEx(g_hMouseHook);
    return 0;
}