#pragma once

#include <cstdint>

#include "imgui.h"
#include "Misc/InputEventHandler.h"

class FSpectrumEmu;

class FSpectrumViewer
{
public:
	FSpectrumViewer() {}

	void	Init(FSpectrumEmu* pEmu);
	void	Draw();
	void	Tick(void);

private:
	FSpectrumEmu* pSpectrumEmu = nullptr;

	// screen inspector
	bool		bScreenCharSelected = false;
	uint16_t	SelectPixAddr = 0;
	uint16_t	SelectAttrAddr = 0;
	int			SelectedCharX = 0;
	int			SelectedCharY = 0;
	bool		CharDataFound = false;
	uint16_t	FoundCharDataAddress = 0;
	uint8_t		CharData[8] = {0};
	bool		bCharSearchWrap = true;
	bool		bWindowFocused = false;
};