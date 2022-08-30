
#include "SpeccyUI.h"
#include <windows.h>

#include "GameConfig.h"
#include <ImGuiSupport/imgui_impl_lucidextra.h>
#include "GameViewers/GameViewer.h"
#include "GameViewers/StarquakeViewer.h"
#include "GameViewers/MiscGameViewers.h"
#include "GraphicsViewer.h"
#include "Util/FileUtil.h"

#include "ui/ui_dbg.h"
#include "MemoryHandlers.h"
#include "FunctionHandlers.h"
#include "Disassembler.h"
#include "CodeAnalyser/CodeAnalyser.h"
#include "CodeAnalyser/CodeAnalyserUI.h"

#include "Speccy/ROMLabels.h"

void DrawCheatsUI(FSpeccyUI *pUI);

class FSpectrumCPUInterface : public ICPUInterface
{
public:
	FSpectrumCPUInterface()
	{
		CPUType = ECPUType::Z80;
	}

	uint8_t		ReadByte(uint16_t address) const override
	{
		return ReadSpeccyByte(pSpeccy, address);
	}
	uint16_t		ReadWord(uint16_t address) const override
	{
		return ReadSpeccyByte(pSpeccy, address) | (ReadSpeccyByte(pSpeccy, address + 1) << 8);
	}
	uint16_t	GetPC(void) override
	{
		return z80_pc(&pSpeccy->CurrentState.cpu);
	}

	void	Break(void) override
	{
		FSpeccyUI* pUI = GetSpeccyUI();
		pUI->UIZX.dbg.dbg.stopped = true;
		pUI->UIZX.dbg.dbg.step_mode = UI_DBG_STEPMODE_NONE;
	}

	void	Continue(void) override
	{
		FSpeccyUI* pUI = GetSpeccyUI();
		pUI->UIZX.dbg.dbg.stopped = false;
		pUI->UIZX.dbg.dbg.step_mode = UI_DBG_STEPMODE_NONE;
	}

	void GraphicsViewerSetAddress(uint16_t address) override
	{
		GraphicsViewerGoToAddress(address);
	}

	bool	ExecThisFrame(void) override
	{
		return pSpeccy->ExecThisFrame;
	}

	void InsertROMLabels(FCodeAnalysisState& state) override
	{
		for (const auto& label : g_RomLabels)
		{
			AddLabel(state, label.Address, label.pLabelName, label.LabelType);

			// run static analysis on all code labels
			if (label.LabelType == LabelType::Code || label.LabelType == LabelType::Function)
				RunStaticCodeAnalysis(state, label.Address);
		}

		for (const auto& label : g_SysVariables)
		{
			AddLabel(state, label.Address, label.pLabelName, LabelType::Data);
			// TODO: Set up data?
		}
	}

	void InsertSystemLabels(FCodeAnalysisState& state) override
	{
		// screen memory start
		AddLabel(state, 0x4000, "ScreenPixels", LabelType::Data);

		FDataInfo* pScreenPixData = state.GetReadDataInfoForAddress(0x4000);
		pScreenPixData->DataType = DataType::Graphics;
		pScreenPixData->Address = 0x4000;
		pScreenPixData->ByteSize = 0x1800;

		AddLabel(state, 0x5800, "ScreenAttributes", LabelType::Data);
		FDataInfo* pScreenAttrData = state.GetReadDataInfoForAddress(0x5800);
		pScreenAttrData->DataType = DataType::Blob;
		pScreenAttrData->Address = 0x5800;
		pScreenAttrData->ByteSize = 0x400;

		// system variables?
	}

	FSpeccy* pSpeccy = nullptr;
};

FSpectrumCPUInterface	SpeccyCPUIF;

/* reboot callback */
static void boot_cb(zx_t* sys, zx_type_t type)
{
	zx_desc_t desc = {}; // TODO
	zx_init(sys, &desc);
}

void* gfx_create_texture(int w, int h)
{
	return ImGui_ImplDX11_CreateTextureRGBA(nullptr, w, h);
}

void gfx_update_texture(void* h, void* data, int data_byte_size)
{
	ImGui_ImplDX11_UpdateTextureRGBA(h, (unsigned char *)data);
}

void gfx_destroy_texture(void* h)
{
	
}




int UITrapCallback(uint16_t pc, int ticks, uint64_t pins, void* user_data)
{
	FSpeccyUI *pUI = (FSpeccyUI *)user_data;
	FCodeAnalysisState &state = pUI->CodeAnalysis;
	const uint16_t addr = Z80_GET_ADDR(pins);
	const bool bMemAccess = !!((pins & Z80_CTRL_MASK) & Z80_MREQ);
	const bool bWrite = (pins & Z80_CTRL_MASK) == (Z80_MREQ | Z80_WR);

	const uint16_t nextpc = pc;
	// store program count in history
	const uint16_t prevPC = pUI->PCHistory[pUI->PCHistoryPos];
	pUI->PCHistoryPos = (pUI->PCHistoryPos + 1) % FSpeccyUI::kPCHistorySize;
	pUI->PCHistory[pUI->PCHistoryPos] = pc;

	pc = prevPC;	// set PC to pc of instruction just executed

	RegisterCodeExecuted(state, pc, nextpc);
	FCodeInfo* pCodeInfo = state.GetCodeInfoForAddress(pc);
	pCodeInfo->FrameLastAccessed = state.CurrentFrameNo;

	// check for breakpointed code line
	if (pCodeInfo->bBreakpointed)
		return UI_DBG_BP_BASE_TRAPID;
	
	int trapId = MemoryHandlerTrapFunction(pc, ticks, pins, pUI);


	//if(trapId == 0)
		//trapId = FunctionTrapFunction(pc,nextpc, ticks, pins, pUI);
	
	
	return trapId;
}

int UIEvalBreakpoint(ui_dbg_t* dbg_win, uint16_t pc, int ticks, uint64_t pins, void* user_data)
{
	return 0;
}

z80_tick_t g_OldTickCB = nullptr;

extern uint16_t g_PC;
uint64_t Z80Tick(int num, uint64_t pins, void* user_data)
{
	FSpeccyUI *pUI = (FSpeccyUI *)user_data;
	FCodeAnalysisState &state = pUI->CodeAnalysis;
	const uint16_t pc = g_PC;// z80_pc(&pUI->pSpeccy->CurrentState.cpu);
	/* memory and IO requests */
	if (pins & Z80_MREQ) 
	{
		/* a memory request machine cycle
			FIXME: 'contended memory' accesses should inject wait states
		*/
		const uint16_t addr = Z80_GET_ADDR(pins);
		if (pins & Z80_RD)
		{
			if (state.bRegisterDataAccesses)
				RegisterDataAccess(state, pc, addr, false);
		}
		else if (pins & Z80_WR) 
		{
			if (state.bRegisterDataAccesses)
				RegisterDataAccess(state, pc, addr, true);

			state.SetLastWriterForAddress(addr,pc);

			FCodeInfo *pCodeWrittenTo = state.GetCodeInfoForAddress(addr);
			if (pCodeWrittenTo != nullptr && pCodeWrittenTo->bSelfModifyingCode == false)
				pCodeWrittenTo->bSelfModifyingCode = true;
		}
	}
	else if (pins & Z80_IORQ)
	{
		IOAnalysisHandler(pUI->IOAnalysis, pc, pins);
	}

	return g_OldTickCB(num, pins, &pUI->pSpeccy->CurrentState);
}

FSpeccyUI*g_pSpeccyUI = nullptr;

FSpeccyUI* InitSpeccyUI(FSpeccy *pSpeccy)
{
	FSpeccyUI *pUI = new FSpeccyUI;
	g_pSpeccyUI = pUI;
	memset(&pUI->UIZX, 0, sizeof(ui_zx_t));

	// Trap callback needs to be set before we create the UI
	z80_trap_cb(&pSpeccy->CurrentState.cpu, UITrapCallback, pUI);

	g_OldTickCB = pSpeccy->CurrentState.cpu.tick_cb;
	pSpeccy->CurrentState.cpu.user_data = pUI;
	pSpeccy->CurrentState.cpu.tick_cb = Z80Tick;

	pUI->pSpeccy = pSpeccy;
	//ui_init(zxui_draw);
	{
		ui_zx_desc_t desc = { 0 };
		desc.zx = &pSpeccy->CurrentState;
		desc.boot_cb = boot_cb;
		desc.create_texture_cb = gfx_create_texture;
		desc.update_texture_cb = gfx_update_texture;
		desc.destroy_texture_cb = gfx_destroy_texture;
		desc.dbg_keys.break_keycode = ImGui::GetKeyIndex(ImGuiKey_Space);
		desc.dbg_keys.break_name = "F5";
		desc.dbg_keys.continue_keycode = VK_F5;
		desc.dbg_keys.continue_name = "F5";
		desc.dbg_keys.step_over_keycode = VK_F6;
		desc.dbg_keys.step_over_name = "F6";
		desc.dbg_keys.step_into_keycode = VK_F7;
		desc.dbg_keys.step_into_name = "F7";
		desc.dbg_keys.toggle_breakpoint_keycode = VK_F9;
		desc.dbg_keys.toggle_breakpoint_name = "F9";
		ui_zx_init(&pUI->UIZX, &desc);
	}

	// additional debugger config
	//pUI->UIZX.dbg.ui.open = true;
	pUI->UIZX.dbg.break_cb = UIEvalBreakpoint;
	

	// Setup Disassembler for function view
	FDasmDesc desc;
	desc.LayerNames[0] = "CPU Mapped";
	desc.LayerNames[1] = "Layer 0";
	desc.LayerNames[2] = "Layer 1";
	desc.LayerNames[3] = "Layer 2";
	desc.LayerNames[4] = "Layer 3";
	desc.LayerNames[5] = "Layer 4";
	desc.LayerNames[6] = "Layer 5";
	desc.LayerNames[7] = "Layer 6";
	desc.CPUType = DasmCPUType::Z80;
	desc.StartAddress = 0x0000;
	desc.ReadCB = MemReadFunc;
	desc.pUserData = &pSpeccy->CurrentState;
	desc.pUI = pUI;
	desc.Title = "FunctionDasm";
	DasmInit(&pUI->FunctionDasm, &desc);

	pUI->GraphicsViewer.pSpeccy = pSpeccy;
	pUI->GraphicsViewer.pUI = pUI;
	InitGraphicsViewer(pUI->GraphicsViewer);
	pUI->IOAnalysis.pCodeAnalysis = &pUI->CodeAnalysis;
	InitIOAnalysis(pUI->IOAnalysis);
	
	// register Viewers
	RegisterStarquakeViewer(pUI);
	RegisterGames(pUI);

	LoadGameConfigs(pUI);

	// Set up code analysis
	FCodeAnalysisState &state = pUI->CodeAnalysis;

	// initialise code analysis pages
	
	// ROM
	for (int pageNo = 0; pageNo < FSpeccyUI::kNoROMPages; pageNo++)
	{
		pUI->ROMPages[pageNo].Initialise(pageNo * FCodeAnalysisPage::kPageSize);
		pUI->CodeAnalysis.SetCodeAnalysisRWPage(pageNo, &pUI->ROMPages[pageNo], &pUI->ROMPages[pageNo]);	// Read Only
	}
	// RAM
	const uint16_t RAMStartAddr = FSpeccyUI::kNoROMPages * FCodeAnalysisPage::kPageSize;
	for (int pageNo = 0; pageNo < FSpeccyUI::kNoRAMPages; pageNo++)
	{
		pUI->RAMPages[pageNo].Initialise(RAMStartAddr + (pageNo * FCodeAnalysisPage::kPageSize));
		pUI->CodeAnalysis.SetCodeAnalysisRWPage(pageNo + FSpeccyUI::kNoROMPages, &pUI->RAMPages[pageNo], &pUI->RAMPages[pageNo]);	// Read/Write
	}

	/*for (int addr = 0; addr < (1 << 16); addr++)
	{
		state.SetLabelForAddress(addr, nullptr);
		state.SetCodeInfoForAddress(addr, nullptr);

		// set up data entry for address
		FDataInfo *pDataInfo = new FDataInfo;
		pDataInfo->Address = (uint16_t)addr;
		pDataInfo->ByteSize = 1;
		pDataInfo->DataType = DataType::Byte;
		state.SetReadDataInfoForAddress(addr, pDataInfo);
	}*/
	//....

	// run initial analysis
	SpeccyCPUIF.pSpeccy = pUI->pSpeccy;
	InitialiseCodeAnalysis(pUI->CodeAnalysis,&SpeccyCPUIF);
	LoadROMData(pUI->CodeAnalysis, "GameData/RomInfo.bin");
	
	return pUI;
}

FSpeccyUI* GetSpeccyUI()
{
	return g_pSpeccyUI;
}

void ShutdownSpeccyUI(FSpeccyUI* pUI)
{
	SaveCurrentGameData(pUI);	// save on close
}



void StartGame(FSpeccyUI* pUI, FGameConfig *pGameConfig)
{
	pUI->MemoryAccessHandlers.clear();	// remove old memory handlers

	ResetMemoryStats(pUI->MemStats);
	
	// Reset Functions
	pUI->FunctionStack.clear();
	pUI->Functions.clear();

	// start up game
	if(pUI->pActiveGame!=nullptr)
		delete pUI->pActiveGame->pViewerData;
	delete pUI->pActiveGame;
	
	FGame *pNewGame = new FGame;
	pNewGame->pConfig = pGameConfig;
	pNewGame->pViewerConfig = pGameConfig->pViewerConfig;
	assert(pGameConfig->pViewerConfig != nullptr);
	pUI->GraphicsViewer.pGame = pNewGame;
	pUI->pActiveGame = pNewGame;
	pNewGame->pViewerData = pNewGame->pViewerConfig->pInitFunction(pUI, pGameConfig);
	GenerateSpriteListsFromConfig(pUI->GraphicsViewer, pGameConfig);
	
	// Initialise code analysis
	SpeccyCPUIF.pSpeccy = pUI->pSpeccy;
	InitialiseCodeAnalysis(pUI->CodeAnalysis, &SpeccyCPUIF);

	// load game data if we can
	std::string dataFName = "GameData/" + pGameConfig->Name + ".bin";
	LoadGameData(pUI->CodeAnalysis, dataFName.c_str());
	LoadROMData(pUI->CodeAnalysis, "GameData/RomInfo.bin");
	ReAnalyseCode(pUI->CodeAnalysis);
	GenerateGlobalInfo(pUI->CodeAnalysis);
}

bool StartGame(FSpeccyUI* pUI, const char *pGameName)
{
	for (const auto& pGameConfig : GetGameConfigs())
	{
		if (pGameConfig->Name == pGameName)
		{
			std::string gameFile = "Games/" + pGameConfig->Z80File;
			if (LoadGameSnapshot(*pUI->pSpeccy, gameFile.c_str()))
			{
				StartGame(pUI, pGameConfig);
				return true;
			}
		}
	}

	return false;
}

// save config & data
void SaveCurrentGameData(FSpeccyUI *pUI)
{
	if (pUI->pActiveGame != nullptr)
	{
		const FGameConfig *pGameConfig = pUI->pActiveGame->pConfig;
		if (pGameConfig->Name.empty())
		{
			
		}
		else
		{
			const std::string configFName = "Configs/" + pGameConfig->Name + ".json";
			const std::string dataFName = "GameData/" + pGameConfig->Name + ".bin";
			EnsureDirectoryExists("Configs");
			EnsureDirectoryExists("GameData");

			SaveGameConfigToFile(*pGameConfig, configFName.c_str());
			SaveGameData(pUI->CodeAnalysis, dataFName.c_str());
		}
	}
	SaveROMData(pUI->CodeAnalysis, "GameData/RomInfo.bin");
}

static void DrawMainMenu(FSpeccyUI* pUI, double timeMS)
{
	ui_zx_t* pZXUI = &pUI->UIZX;
	FSpeccy *pSpeccy = pUI->pSpeccy;
	assert(pZXUI && pZXUI->zx && pZXUI->boot_cb);
		
	if (ImGui::BeginMainMenuBar()) 
	{
		if (ImGui::BeginMenu("File"))
		{
			if (ImGui::BeginMenu("New Game from Snapshot File"))
			{
				for (const auto& game : GetGameList())
				{
					if (ImGui::MenuItem(game.c_str()))
					{
						const std::string gameFile = "Games/" + game;
						if (LoadGameSnapshot(*pSpeccy, gameFile.c_str()))
						{
							FGameConfig *pNewConfig = CreateNewGameConfigFromZ80File(game.c_str());
							if(pNewConfig != nullptr)
								StartGame(pUI, pNewConfig);
						}
					}
				}

				ImGui::EndMenu();
			}

			if (ImGui::BeginMenu( "Open Game"))
			{
				for (const auto& pGameConfig : GetGameConfigs())
				{
					if (ImGui::MenuItem(pGameConfig->Name.c_str()))
					{
						const std::string gameFile = "Games/" + pGameConfig->Z80File;

						if(LoadGameSnapshot(*pSpeccy, gameFile.c_str()))
						{
							StartGame(pUI,pGameConfig);
						}
					}
				}

				ImGui::EndMenu();
			}

			if (ImGui::MenuItem("Open POK File..."))
			{
				std::string pokFile;
				OpenFileDialog(pokFile, ".\\POKFiles", "POK\0*.pok\0");
			}
			
			if (ImGui::MenuItem("Save Game Data"))
			{
				SaveCurrentGameData(pUI);
			}
			if (ImGui::MenuItem("Export Binary File"))
			{
				if (pUI->pActiveGame != nullptr)
				{
					EnsureDirectoryExists("OutputBin/");
					std::string outBinFname = "OutputBin/" + pUI->pActiveGame->pConfig->Name + ".bin";
					uint8_t *pSpecMem = new uint8_t[65536];
					for (int i = 0; i < 65536; i++)
						pSpecMem[i] = ReadSpeccyByte(pSpeccy, i);
					SaveBinaryFile(outBinFname.c_str(), pSpecMem, 65536);
					delete pSpecMem;
				}
			}

			if (ImGui::MenuItem("Export ASM File"))
			{
				if (pUI->pActiveGame != nullptr)
				{
					EnsureDirectoryExists("OutputASM/");
					std::string outBinFname = "OutputASM/" + pUI->pActiveGame->pConfig->Name + ".asm";

					OutputCodeAnalysisToTextFile(pUI->CodeAnalysis, outBinFname.c_str(),0x4000,0xffff);
				}
			}

			// TODO: export data for skookit
			if (ImGui::MenuItem("Export Region Info File"))
			{
			}
			ImGui::EndMenu();
		}
		
		if (ImGui::BeginMenu("System")) 
		{
			if (ImGui::MenuItem("Reset")) 
			{
				zx_reset(pZXUI->zx);
				ui_dbg_reset(&pZXUI->dbg);
			}
			/*if (ImGui::MenuItem("ZX Spectrum 48K", 0, (pZXUI->zx->type == ZX_TYPE_48K)))
			{
				pZXUI->boot_cb(pZXUI->zx, ZX_TYPE_48K);
				ui_dbg_reboot(&pZXUI->dbg);
			}
			if (ImGui::MenuItem("ZX Spectrum 128", 0, (pZXUI->zx->type == ZX_TYPE_128)))
			{
				pZXUI->boot_cb(pZXUI->zx, ZX_TYPE_128);
				ui_dbg_reboot(&pZXUI->dbg);
			}*/
			if (ImGui::BeginMenu("Joystick")) 
			{
				if (ImGui::MenuItem("None", 0, (pZXUI->zx->joystick_type == ZX_JOYSTICKTYPE_NONE)))
				{
					pZXUI->zx->joystick_type = ZX_JOYSTICKTYPE_NONE;
				}
				if (ImGui::MenuItem("Kempston", 0, (pZXUI->zx->joystick_type == ZX_JOYSTICKTYPE_KEMPSTON)))
				{
					pZXUI->zx->joystick_type = ZX_JOYSTICKTYPE_KEMPSTON;
				}
				if (ImGui::MenuItem("Sinclair #1", 0, (pZXUI->zx->joystick_type == ZX_JOYSTICKTYPE_SINCLAIR_1)))
				{
					pZXUI->zx->joystick_type = ZX_JOYSTICKTYPE_SINCLAIR_1;
				}
				if (ImGui::MenuItem("Sinclair #2", 0, (pZXUI->zx->joystick_type == ZX_JOYSTICKTYPE_SINCLAIR_2)))
				{
					pZXUI->zx->joystick_type = ZX_JOYSTICKTYPE_SINCLAIR_2;
				}
				ImGui::EndMenu();
			}
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Hardware")) 
		{
			ImGui::MenuItem("Memory Map", 0, &pZXUI->memmap.open);
			ImGui::MenuItem("Keyboard Matrix", 0, &pZXUI->kbd.open);
			ImGui::MenuItem("Audio Output", 0, &pZXUI->audio.open);
			ImGui::MenuItem("Z80 CPU", 0, &pZXUI->cpu.open);
			if (pZXUI->zx->type == ZX_TYPE_128)
			{
				ImGui::MenuItem("AY-3-8912", 0, &pZXUI->ay.open);
			}
			else 
			{
				pZXUI->ay.open = false;
			}
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Debug")) 
		{
			//ImGui::MenuItem("CPU Debugger", 0, &pZXUI->dbg.ui.open);
			ImGui::MenuItem("Breakpoints", 0, &pZXUI->dbg.ui.show_breakpoints);
			ImGui::MenuItem("Memory Heatmap", 0, &pZXUI->dbg.ui.show_heatmap);
			if (ImGui::BeginMenu("Memory Editor")) 
			{
				ImGui::MenuItem("Window #1", 0, &pZXUI->memedit[0].open);
				ImGui::MenuItem("Window #2", 0, &pZXUI->memedit[1].open);
				ImGui::MenuItem("Window #3", 0, &pZXUI->memedit[2].open);
				ImGui::MenuItem("Window #4", 0, &pZXUI->memedit[3].open);
				ImGui::EndMenu();
			}
			/*if (ImGui::BeginMenu("Disassembler")) 
			{
				ImGui::MenuItem("Window #1", 0, &pZXUI->dasm[0].open);
				ImGui::MenuItem("Window #2", 0, &pZXUI->dasm[1].open);
				ImGui::MenuItem("Window #3", 0, &pZXUI->dasm[2].open);
				ImGui::MenuItem("Window #4", 0, &pZXUI->dasm[3].open);
				ImGui::EndMenu();
			}*/
			ImGui::EndMenu();
		}
		/*if (ImGui::BeginMenu("ImGui"))
		{
			ImGui::MenuItem("Show Demo", 0, &pUI->bShowImGuiDemo);
			ImGui::EndMenu();
		}*/
		

		/*if (ImGui::BeginMenu("Game Viewers"))
		{
			for (auto &viewerIt : pUI->GameViewers)
			{
				FGameViewer &viewer = viewerIt.second;
				ImGui::MenuItem(viewerIt.first.c_str(), 0, &viewer.bOpen);
			}
			ImGui::EndMenu();
		}*/
		
		//ui_util_options_menu(timeMS, pZXUI->dbg.dbg.stopped);

		// draw emu timings
		ImGui::SameLine(ImGui::GetWindowWidth() - 120);
		if (pZXUI->dbg.dbg.stopped) 
			ImGui::Text("emu: stopped");
		else 
			ImGui::Text("emu: %.2fms", timeMS);

		ImGui::EndMainMenuBar();
	}

}

static void UpdateMemmap(ui_zx_t* ui)
{
	assert(ui && ui->zx);
	ui_memmap_reset(&ui->memmap);
	if (ZX_TYPE_48K == ui->zx->type) 
	{
		ui_memmap_layer(&ui->memmap, "System");
		ui_memmap_region(&ui->memmap, "ROM", 0x0000, 0x4000, true);
		ui_memmap_region(&ui->memmap, "RAM", 0x4000, 0xC000, true);
	}
	else 
	{
		const uint8_t m = ui->zx->last_mem_config;
		ui_memmap_layer(&ui->memmap, "Layer 0");
		ui_memmap_region(&ui->memmap, "ZX128 ROM", 0x0000, 0x4000, !(m & (1 << 4)));
		ui_memmap_region(&ui->memmap, "RAM 5", 0x4000, 0x4000, true);
		ui_memmap_region(&ui->memmap, "RAM 2", 0x8000, 0x4000, true);
		ui_memmap_region(&ui->memmap, "RAM 0", 0xC000, 0x4000, 0 == (m & 7));
		ui_memmap_layer(&ui->memmap, "Layer 1");
		ui_memmap_region(&ui->memmap, "ZX48K ROM", 0x0000, 0x4000, 0 != (m & (1 << 4)));
		ui_memmap_region(&ui->memmap, "RAM 1", 0xC000, 0x4000, 1 == (m & 7));
		ui_memmap_layer(&ui->memmap, "Layer 2");
		ui_memmap_region(&ui->memmap, "RAM 2", 0xC000, 0x4000, 2 == (m & 7));
		ui_memmap_layer(&ui->memmap, "Layer 3");
		ui_memmap_region(&ui->memmap, "RAM 3", 0xC000, 0x4000, 3 == (m & 7));
		ui_memmap_layer(&ui->memmap, "Layer 4");
		ui_memmap_region(&ui->memmap, "RAM 4", 0xC000, 0x4000, 4 == (m & 7));
		ui_memmap_layer(&ui->memmap, "Layer 5");
		ui_memmap_region(&ui->memmap, "RAM 5", 0xC000, 0x4000, 5 == (m & 7));
		ui_memmap_layer(&ui->memmap, "Layer 6");
		ui_memmap_region(&ui->memmap, "RAM 6", 0xC000, 0x4000, 6 == (m & 7));
		ui_memmap_layer(&ui->memmap, "Layer 7");
		ui_memmap_region(&ui->memmap, "RAM 7", 0xC000, 0x4000, 7 == (m & 7));
	}
}

void DrawDebuggerUI(ui_dbg_t *pDebugger)
{
	if (ImGui::Begin("CPU Debugger"))
	{
		ui_dbg_dbgwin_draw(pDebugger);
	}
	ImGui::End();
	ui_dbg_draw(pDebugger);
	/*
	if (!(pDebugger->ui.open || pDebugger->ui.show_heatmap || pDebugger->ui.show_breakpoints)) {
		return;
	}
	_ui_dbg_dbgwin_draw(pDebugger);
	_ui_dbg_heatmap_draw(pDebugger);
	_ui_dbg_bp_draw(pDebugger);*/
}

void UpdatePreTickSpeccyUI(FSpeccyUI* pUI)
{
	pUI->pSpeccy->ExecThisFrame = ui_zx_before_exec(&pUI->UIZX);
}





void DrawMemoryTools(FSpeccyUI* pUI)
{
	if (ImGui::Begin("Memory Tools") == false)
	{
		ImGui::End();
		return;
	}
	if (ImGui::BeginTabBar("MemoryToolsTabBar"))
	{

		if (ImGui::BeginTabItem("Memory Handlers"))
		{
			DrawMemoryHandlers(pUI);
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem("Memory Analysis"))
		{
			DrawMemoryAnalysis(pUI);
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Memory Diff"))
		{
			DrawMemoryDiffUI(pUI);
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("IO Analysis"))
		{
			DrawIOAnalysis(pUI->IOAnalysis);
			ImGui::EndTabItem();
		}
		
		/*if (ImGui::BeginTabItem("Functions"))
		{
			DrawFunctionInfo(pUI);
			ImGui::EndTabItem();
		}*/

		ImGui::EndTabBar();
	}

	ImGui::End();
}


void ReadSpeccyKeys(FSpeccy *pSpeccy)
{
	ImGuiIO &io = ImGui::GetIO();
	for(int i=0;i<10;i++)
	{
		if (io.KeysDown[0x30+i] == 1)
		{
			zx_key_down(&pSpeccy->CurrentState, '0' + i);
			zx_key_up(&pSpeccy->CurrentState, '0' + i);
		}
	}

	for (int i = 0; i < 26; i++)
	{
		if (io.KeysDown[0x41 + i] == 1)
		{
			zx_key_down(&pSpeccy->CurrentState, 'a' + i);
			zx_key_up(&pSpeccy->CurrentState, 'a' + i);
		}
	}
}


void DrawSpeccyUI(FSpeccyUI* pUI)
{
	ui_zx_t* pZXUI = &pUI->UIZX;
	FSpeccy *pSpeccy = pUI->pSpeccy;
	const double timeMS = 1000.0f / ImGui::GetIO().Framerate;
	
	if(pSpeccy->ExecThisFrame)
		ui_zx_after_exec(pZXUI);
	
	DrawMainMenu(pUI, timeMS);
	if (pZXUI->memmap.open)
	{
		UpdateMemmap(pZXUI);
	}

	// call the Chips UI functions
	ui_audio_draw(&pZXUI->audio, pZXUI->zx->sample_pos);
	ui_z80_draw(&pZXUI->cpu);
	ui_ay38910_draw(&pZXUI->ay);
	ui_kbd_draw(&pZXUI->kbd);
	ui_memmap_draw(&pZXUI->memmap);

	for (int i = 0; i < 4; i++)
	{
		ui_memedit_draw(&pZXUI->memedit[i]);
		ui_dasm_draw(&pZXUI->dasm[i]);
	}

	DrawDebuggerUI(&pZXUI->dbg);

	//DasmDraw(&pUI->FunctionDasm);


	// show spectrum window
	if (ImGui::Begin("Spectrum View"))
	{
		ImGuiIO& io = ImGui::GetIO();
		ImVec2 pos = ImGui::GetCursorScreenPos();
		ImGui::Image(pSpeccy->Texture, ImVec2(320, 256));
		if (ImGui::IsItemHovered())
		{
			const int borderOffsetX = (320 - 256) / 2;
			const int borderOffsetY = (256 - 192) / 2;
			const int xp = min(max((int)(io.MousePos.x - pos.x - borderOffsetX),0),255);
			const int yp = min(max((int)(io.MousePos.y - pos.y - borderOffsetY), 0), 191);
			
			const uint16_t scrPixAddress = GetScreenPixMemoryAddress(xp, yp);
			const uint16_t scrAttrAddress = GetScreenAttrMemoryAddress(xp, yp);

			if(scrPixAddress!=0)
			{
				ImDrawList* dl = ImGui::GetWindowDrawList();
				const int rx = static_cast<int>(pos.x) + borderOffsetX + (xp & ~0x7);
				const int ry = static_cast<int>(pos.y) + borderOffsetY + (yp & ~0x7);
				dl->AddRect(ImVec2(rx,ry),ImVec2(rx+8,ry+8),0xffffffff);
				ImGui::BeginTooltip();
				ImGui::Text("Screen Pos (%d,%d)", xp, yp);
				ImGui::Text("Pixel: %04Xh, Attr: %04Xh", scrPixAddress, scrAttrAddress);

				const uint16_t lastPixWriter = pUI->CodeAnalysis.GetLastWriterForAddress(scrPixAddress);
				const uint16_t lastAttrWriter = pUI->CodeAnalysis.GetLastWriterForAddress(scrAttrAddress);
				ImGui::Text("Pixel Writer: ");
				ImGui::SameLine();
				DrawCodeAddress(pUI->CodeAnalysis, lastPixWriter);
				ImGui::Text("Attribute Writer: ");
				ImGui::SameLine();
				DrawCodeAddress(pUI->CodeAnalysis, lastAttrWriter);
				ImGui::EndTooltip();
				//ImGui::Text("Pixel Writer: %04X, Attrib Writer: %04X", lastPixWriter, lastAttrWriter);

				if(ImGui::IsMouseClicked(0))
				{
					pUI->bScreenCharSelected = true;
					pUI->SelectedCharX = rx;
					pUI->SelectedCharY = ry;
					pUI->SelectPixAddr = scrPixAddress;
					pUI->SelectAttrAddr = scrAttrAddress;
				}

				if (ImGui::IsMouseClicked(1))
					pUI->bScreenCharSelected = false;

				if (ImGui::IsMouseDoubleClicked(0))
					CodeAnalyserGoToAddress(pUI->CodeAnalysis, lastPixWriter);
				if (ImGui::IsMouseDoubleClicked(1))
					CodeAnalyserGoToAddress(pUI->CodeAnalysis, lastAttrWriter);
			}
			
		}

		if(pUI->bScreenCharSelected == true)
		{
			ImDrawList* dl = ImGui::GetWindowDrawList();
			const ImU32 col = 0xffffffff;	// TODO: pulse
			dl->AddRect(ImVec2(pUI->SelectedCharX, pUI->SelectedCharY), ImVec2(pUI->SelectedCharX + 8, pUI->SelectedCharY + 8), col);

			ImGui::Text("Pixel Char Address:");
			ImGui::SameLine();
			DrawAddressLabel(pUI->CodeAnalysis, pUI->SelectPixAddr);
			ImGui::Text("Attribute Address:");
			ImGui::SameLine();
			DrawAddressLabel(pUI->CodeAnalysis, pUI->SelectAttrAddr);
		}
		
		ImGui::SliderFloat("Speed Scale", &pSpeccy->ExecSpeedScale, 0.0f, 1.0f);
		ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);
		// read keys
		if (ImGui::IsWindowFocused())
		{
			ReadSpeccyKeys(pUI->pSpeccy);
		}
	}
	ImGui::End();

	// cheats 
	if (ImGui::Begin("Cheats"))
	{
		DrawCheatsUI(pUI);
	}
	ImGui::End();

	// game viewer
	if (ImGui::Begin("Game Viewer"))
	{
		if (pUI->pActiveGame != nullptr)
		{
			ImGui::Text(pUI->pActiveGame->pConfig->Name.c_str());
			pUI->pActiveGame->pViewerConfig->pDrawFunction(pUI, pUI->pActiveGame);
		}
		
	}
	ImGui::End();

	DrawGraphicsViewer(pUI->GraphicsViewer);
	DrawMemoryTools(pUI);
	if (ImGui::Begin("Code Analysis"))
	{
		DrawCodeAnalysisData(pUI->CodeAnalysis);
	}
	ImGui::End();

	/*if (ImGui::Begin("Globals"))
	{
		DrawGlobals(pUI->CodeAnalysis);
		ImGui::End();
	}*/
}

bool DrawDockingView(FSpeccyUI *pUI)
{
	//SCOPE_PROFILE_CPU("UI", "DrawUI", ProfCols::UI);

	static bool opt_fullscreen_persistant = true;
	bool opt_fullscreen = opt_fullscreen_persistant;
	//static ImGuiDockNodeFlags dockspace_flags = ImGuiDockNodeFlags_PassthruCentralNode;
	bool bOpen = false;
	ImGuiDockNodeFlags dockFlags = 0;

	// We are using the ImGuiWindowFlags_NoDocking flag to make the parent window not dockable into,
	// because it would be confusing to have two docking targets within each others.
	ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
	if (opt_fullscreen)
	{
		ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(viewport->Pos);
		ImGui::SetNextWindowSize(viewport->Size);
		ImGui::SetNextWindowViewport(viewport->ID);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
		window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
		window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
	}

	// When using ImGuiDockNodeFlags_PassthruCentralNode, DockSpace() will render our background and handle the pass-thru hole, so we ask Begin() to not render a background.
	if (dockFlags & ImGuiDockNodeFlags_PassthruCentralNode)
		window_flags |= ImGuiWindowFlags_NoBackground;

	// Important: note that we proceed even if Begin() returns false (aka window is collapsed).
	// This is because we want to keep our DockSpace() active. If a DockSpace() is inactive, 
	// all active windows docked into it will lose their parent and become undocked.
	// We cannot preserve the docking relationship between an active window and an inactive docking, otherwise 
	// any change of dockspace/settings would lead to windows being stuck in limbo and never being visible.
	bool bQuit = false;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	if (ImGui::Begin("DockSpace Demo", &bOpen, window_flags))
	{
		ImGui::PopStyleVar();

		if (opt_fullscreen)
			ImGui::PopStyleVar(2);

		// DockSpace
		ImGuiIO& io = ImGui::GetIO();
		if (io.ConfigFlags & ImGuiConfigFlags_DockingEnable)
		{
			const ImGuiID dockspaceId = ImGui::GetID("MyDockSpace");
			ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), dockFlags);
		}

		//bQuit = MainMenu();
		//DrawDebugWindows(uiState);
		DrawSpeccyUI(pUI);
		ImGui::End();
	}
	else
	{
		ImGui::PopStyleVar();
		bQuit = true;
	}

	return bQuit;
}

void UpdatePostTickSpeccyUI(FSpeccyUI* pUI)
{
	DrawDockingView(pUI);
	
}

// Cheats

void DrawCheatsUI(FSpeccyUI *pUI)
{
	if (pUI->pActiveGame == nullptr)
		return;
	
	FGameConfig &config = *pUI->pActiveGame->pConfig;

	for (FCheat &cheat : config.Cheats)
	{
		ImGui::PushID(cheat.Description.c_str());
		ImGui::Text(cheat.Description.c_str());
		ImGui::SameLine();
		if (ImGui::Checkbox("##cheatBox", &cheat.bEnabled))
		{
			for (auto &entry : cheat.Entries)
			{
				if (cheat.bEnabled)	// cheat activated
				{
					// store old value
					entry.OldValue = ReadSpeccyByte(pUI->pSpeccy, entry.Address);
					WriteSpeccyByte(pUI->pSpeccy, entry.Address, entry.Value);
				}
				else
				{
					WriteSpeccyByte(pUI->pSpeccy, entry.Address, entry.OldValue);
				}
			}
		}
		ImGui::PopID();
	}
}
