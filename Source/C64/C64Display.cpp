#include "C64Display.h"

#include <chips/m6502.h>
#include <chips/m6526.h>
#include <chips/m6569.h>
#include <chips/m6581.h>
#include <chips/beeper.h>
#include <chips/kbd.h>
#include <chips/mem.h>
#include <chips/clk.h>
#include <systems/c64.h>

#include <imgui.h>
#include <ImGuiSupport/imgui_impl_lucidextra.h>
#include <CodeAnalyser/CodeAnalyser.h>
#include <CodeAnalyser/CodeAnalyserUI.h>
#include <algorithm>

void FC64Display::Init(FCodeAnalysisState* pAnalysis, void* pC64Emu)
{
	C64Emu = pC64Emu;
	CodeAnalysis = pAnalysis;

    // setup pixel buffer
    FramePixelBufferSize = c64_max_display_size();
    FramePixelBuffer = new unsigned char[FramePixelBufferSize * 2];

    // setup texture
    FrameBufferTexture = ImGui_ImplDX11_CreateTextureRGBA(static_cast<unsigned char*>(FramePixelBuffer), c64_std_display_width(), c64_std_display_height());
    //DebugFrameBufferTexture = ImGui_ImplDX11_CreateTextureRGBA(static_cast<unsigned char*>(FramePixelBuffer), _C64_DBG_DISPLAY_WIDTH, _C64_DBG_DISPLAY_HEIGHT);

}

uint16_t FC64Display::GetScreenBitmapAddress(int pixelX, int pixelY)
{
    c64_t* pC64 = (c64_t*)C64Emu;
    const int charX = pixelX >> 3;
    const int charY = pixelY >> 3;

    uint16_t vicMemBase = pC64->vic_bank_select;
    //uint16_t screenMem = (pC64->vic.reg.mem_ptrs >> 4) << 10;
    uint16_t bitmapMem = ((pC64->vic.reg.mem_ptrs >> 3) & 1) << 13;

    const uint16_t pixLocation = (charY * 320) + (charX * 8) + (pixelY & 7);
    return vicMemBase + bitmapMem + pixLocation;
}

uint16_t FC64Display::GetScreenCharAddress(int pixelX, int pixelY)
{
    c64_t* pC64 = (c64_t*)C64Emu;
    const int charX = pixelX >> 3;
    const int charY = pixelY >> 3;

    uint16_t vicMemBase = pC64->vic_bank_select;
    uint16_t screenMem = ((pC64->vic.reg.mem_ptrs >> 4) & 7) << 10;

    const uint16_t charLocation = (charY * 40) + charX;
    return vicMemBase + screenMem + charLocation;
}

uint16_t FC64Display::GetColourRAMAddress(int pixelX, int pixelY)
{
    const int charX = pixelX >> 3;
    const int charY = pixelY >> 3;
    return 0xD800 + (charY * 40) + charX;
}

void FC64Display::DrawUI()
{
    c64_t* pC64 = (c64_t*)C64Emu;

    const bool bDebugFrame = pC64->vic.debug_vis;
    //if (bDebugFrame)
    //else
      //  ImGui_ImplDX11_UpdateTextureRGBA(FrameBufferTexture, FramePixelBuffer, _C64_STD_DISPLAY_WIDTH, _C64_STD_DISPLAY_HEIGHT);
    const int xScrollOff = pC64->vic.reg.ctrl_2 & 7;
    const int yScrollOff = pC64->vic.reg.ctrl_1 & 7;
    const int screenWidthChars = 40;// pC64->vic.reg.ctrl_2& (1 << 3) ? 40 : 38;
    const int screenHeightChars = 25;// pC64->vic.reg.ctrl_1& (1 << 3) ? 25 : 24;
    const int graphicsScreenWidth = screenWidthChars * 8;
    const int graphicsScreenHeight = screenHeightChars * 8;
    const int dispFrameWidth = c64_display_width(pC64);// bDebugFrame ? _C64_DBG_DISPLAY_WIDTH : _C64_STD_DISPLAY_WIDTH;
    const int dispFrameHeight = c64_display_height(pC64);// bDebugFrame ? _C64_DBG_DISPLAY_HEIGHT : _C64_STD_DISPLAY_HEIGHT;

    ImGui_ImplDX11_UpdateTextureRGBA(FrameBufferTexture, FramePixelBuffer, dispFrameWidth, dispFrameHeight);

    ImGui::Text("Frame buffer size = %d x %d", dispFrameWidth, dispFrameHeight);

    const bool bBitmapMode = !!(pC64->vic.reg.ctrl_1 & (1 << 5));
    const bool bExtendedBackgroundMode = !!(pC64->vic.reg.ctrl_1 & (1 << 6));
    const bool bMultiColourMode = !!(pC64->vic.reg.ctrl_2 & (1<<4));
    uint16_t vicMemBase = pC64->vic_bank_select;
    uint16_t screenMem = (pC64->vic.reg.mem_ptrs >> 4) << 10;
    ImGui::Text("VIC Bank: $%04X, Screen Mem: $%04X", vicMemBase, vicMemBase + screenMem);
    if (bBitmapMode)
    {
        uint16_t bitmapMem = ((pC64->vic.reg.mem_ptrs >> 2) & 1) << 13;
        ImGui::SameLine();
        ImGui::Text(", Bitmap Address: $%04X", vicMemBase + bitmapMem);
    }
    else
    {
        uint16_t charDefs = ((pC64->vic.reg.mem_ptrs >> 1) & 7) << 11;
        ImGui::SameLine();
        ImGui::Text(", Character Defs: $%04X", vicMemBase + charDefs);
    }
    ImGui::Text("%s Mode, Multi Colour %s, ECBM:%s", bBitmapMode ? "Bitmap" : "Character", bMultiColourMode ? "Yes" : "No", bExtendedBackgroundMode ? "Yes" : "No");

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::Image(FrameBufferTexture, ImVec2((float)dispFrameWidth, (float)dispFrameHeight));

    if (ImGui::IsItemHovered())
    {
        const int borderOffsetX = ((((dispFrameWidth>>3) - (graphicsScreenWidth>>3)) / 2) << 3) + xScrollOff;
        const int borderOffsetY = ((((dispFrameHeight>>3) - (graphicsScreenHeight>>3)) / 2) << 3) + yScrollOff;
        const int xp = std::min(std::max((int)(io.MousePos.x - pos.x - borderOffsetX), 0), graphicsScreenWidth - 1);
        const int yp = std::min(std::max((int)(io.MousePos.y - pos.y - borderOffsetY), 0), graphicsScreenHeight - 1);
        
        dl->AddRect(
            ImVec2((float)pos.x + borderOffsetX, (float)pos.y + borderOffsetY), 
            ImVec2((float)pos.x + borderOffsetX + graphicsScreenWidth, (float)pos.y + borderOffsetY + graphicsScreenHeight), 
            0xffffffff);

        dl->AddRect(
            ImVec2((float)pos.x, (float)pos.y),
            ImVec2((float)pos.x + dispFrameWidth, (float)pos.y + dispFrameHeight),
            0xffffffff);

        const uint16_t scrBitmapAddress = GetScreenBitmapAddress(xp, yp);
        const uint16_t scrCharAddress = GetScreenCharAddress(xp, yp);
        const uint16_t scrColourRamAddress = GetColourRAMAddress(xp, yp);

        if (scrCharAddress != 0)
        {
            const int rx = static_cast<int>(pos.x) + borderOffsetX + (xp & ~0x7);
            const int ry = static_cast<int>(pos.y) + borderOffsetY + (yp & ~0x7);
            dl->AddRect(ImVec2((float)rx, (float)ry), ImVec2((float)rx + 8, (float)ry + 8), 0xffffffff);
            ImGui::BeginTooltip();
            ImGui::Text("Screen Pos (%d,%d)", xp, yp);
            if(bBitmapMode)
                ImGui::Text("Pixel: $%04X, Colour Char: $%04X Colour Ram: $%04X", scrBitmapAddress, scrCharAddress, scrColourRamAddress);
            else
                ImGui::Text("Char: $%04X Colour Ram: $%04X", scrCharAddress, scrColourRamAddress);

            const uint16_t lastBitmapWriter = CodeAnalysis->GetLastWriterForAddress(scrBitmapAddress);
            const uint16_t lastCharWriter = CodeAnalysis->GetLastWriterForAddress(scrCharAddress);
            const uint16_t lastColourRamWriter = CodeAnalysis->GetLastWriterForAddress(scrColourRamAddress);

            if (bBitmapMode)
            {
                ImGui::Text("Bitmap Writer: ");
                ImGui::SameLine();
                DrawCodeAddress(*CodeAnalysis, lastBitmapWriter);
            }
            ImGui::Text("Char Writer: ");
            ImGui::SameLine();
            DrawCodeAddress(*CodeAnalysis, lastCharWriter);
            ImGui::Text("Colour RAM Writer: ");
            ImGui::SameLine();
            DrawCodeAddress(*CodeAnalysis, lastColourRamWriter);
            ImGui::EndTooltip();
            //ImGui::Text("Pixel Writer: %04X, Attrib Writer: %04X", lastPixWriter, lastAttrWriter);

            if (ImGui::IsMouseClicked(0))
            {
                bScreenCharSelected = true;
                SelectedCharX = rx;
                SelectedCharY = ry;
                SelectBitmapAddr = scrBitmapAddress;
                SelectCharAddr = scrCharAddress;
                SelectColourRamAddr = scrColourRamAddress;
            }

            if (ImGui::IsMouseClicked(1))
                bScreenCharSelected = false;

            if (ImGui::IsMouseDoubleClicked(0))
            {
                if(bBitmapMode)
                    CodeAnalyserGoToAddress(*CodeAnalysis, lastBitmapWriter);
                else
                    CodeAnalyserGoToAddress(*CodeAnalysis, lastCharWriter);
            }
            if (ImGui::IsMouseDoubleClicked(1))
                CodeAnalyserGoToAddress(*CodeAnalysis, lastColourRamWriter);
        }

    }

    if (bScreenCharSelected == true)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 col = 0xffffffff;	// TODO: pulse
        dl->AddRect(ImVec2((float)SelectedCharX, (float)SelectedCharY), ImVec2((float)SelectedCharX + 8, (float)SelectedCharY + 8), col);

        if (bBitmapMode)
        {
            const uint16_t lastBitmapWriter = CodeAnalysis->GetLastWriterForAddress(SelectBitmapAddr);
            ImGui::Text("Bitmap Address: $%X, Last Writer:", SelectBitmapAddr);
            DrawAddressLabel(*CodeAnalysis, lastBitmapWriter);
        }
        const uint16_t lastCharWriter = CodeAnalysis->GetLastWriterForAddress(SelectCharAddr);
        ImGui::Text("Char Address: $%X, Last Writer:", SelectCharAddr);
        DrawAddressLabel(*CodeAnalysis, lastCharWriter);

        const uint16_t lastColourRamWriter = CodeAnalysis->GetLastWriterForAddress(SelectColourRamAddr);
        ImGui::Text("Colour RAM Address: $X, Last Writer:", SelectColourRamAddr);
        DrawAddressLabel(*CodeAnalysis, lastColourRamWriter);
    }
}

