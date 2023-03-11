#pragma once
#include <fstream>
#include <iostream>
#include <filesystem>
#include <unordered_set>
#include <moar_ptr.h>
#include <API/ARK/Ark.h>
#include <Timer.h>
#include "json.hpp"

namespace ArkAutoBosses
{
	using json = nlohmann::json;

	static auto ReadJSON(std::filesystem::path&& path) -> json
	{
		const auto error = fmt::format("Unable to read file {}", std::filesystem::absolute(path).string());
		json read;
		if (!std::filesystem::exists(path))
			throw std::runtime_error(error);

		std::ifstream file{ std::filesystem::absolute(path).string() };
		if (!file.is_open())
			throw std::runtime_error(error);

		file >> read;
		return read;
	}

	class DefeatedBoss
	{
	public:
		std::string Boss;
		std::string Blueprint;
		std::uint32_t Difficulty;
		bool operator==(const DefeatedBoss& other) const
		{
			return this->Difficulty == other.Difficulty && this->Boss == other.Boss && this->Blueprint == other.Blueprint;
		}
		struct Hash
		{
			[[nodiscard]] auto operator()(ArkAutoBosses::DefeatedBoss const& a) const noexcept { return std::hash<std::string>{}(a.Boss + a.Blueprint + std::to_string(a.Difficulty)); }
		};
	};

	class Config
	{
	public:
		std::uint32_t chibiLevels = 0;
		std::unordered_set<DefeatedBoss,DefeatedBoss::Hash> defeatedBosses = {};
		std::unordered_set<std::string> additionalEngrams = {};
	};

	static void from_json(const json& json, DefeatedBoss& defeatedBoss)
	{
		json.at("Boss").get_to(defeatedBoss.Boss);
		json.at("Blueprint").get_to(defeatedBoss.Blueprint);
		json.at("Difficulty").get_to(defeatedBoss.Difficulty);
	}

	static void from_json(const json& json, Config& config)
	{
		json.at("ChibiLevels").get_to(config.chibiLevels);
		json.at("DefeatedBosses").get_to(config.defeatedBosses);
		json.at("AdditionalEngrams").get_to(config.additionalEngrams);
	}

	class Plugin
	{
		static inline moar::function_ptr<void(AShooterGameMode*, APlayerController*, bool, bool, const FPrimalPlayerCharacterConfigStruct&, UPrimalPlayerData*)> StartNewShooterPlayer;
		static inline Config config;

		static inline std::unordered_set<UClass*> unlocksCache;


		static auto CacheUnlocks() -> void
		{
			for (auto boss : config.defeatedBosses)
			{
				FString bossText = FString(boss.Blueprint.c_str());
				UClass* bossClass = UVictoryCore::BPLoadClass(&bossText);


				if (!bossClass)
					continue;

				APrimalDinoCharacter* bossCharacter = static_cast<APrimalDinoCharacter*>(bossClass->GetDefaultObject(true));

				if (!bossCharacter)
					continue; //unable to load this class, skip.

				for (auto&& engram : bossCharacter->DeathGiveEngramClassesField())
					unlocksCache.emplace(engram.uClass);
			}
			for (auto engram : config.additionalEngrams)
			{
				FString engramText = FString(engram.c_str());
				UClass* engramClass = UVictoryCore::BPLoadClass(&engramText);

				if (!engramClass)
					continue;

				unlocksCache.emplace(engramClass);
			}
			Log::GetLog()->info("Cached {} total unlocks.\n", unlocksCache.size());
		}


		static auto SetChibi(AShooterPlayerController* playerController, int value) -> void
		{
			AShooterCharacter* playerPawn = playerController->LastControlledPlayerCharacterField().Get();
			UPrimalPlayerData* playerData = playerController->GetShooterPlayerState()->MyPlayerDataField();

			if (!playerPawn || !playerData) return;

			static UProperty* prop = playerData->FindProperty(FName("NumChibiLevelUpsData", EFindName::FNAME_Find));
			static UFunction* setChibiFunction = playerPawn->FindFunctionChecked(FName("SetNumChibiLevelUps", EFindName::FNAME_Add));

			if (!prop) return;

			playerPawn->ProcessEvent(setChibiFunction, &value);

			prop->Set(playerData, value);
			playerData->SavePlayerData(ArkApi::GetApiUtils().GetWorld());
		}
		static auto DefeatBoss(AShooterPlayerController* playerController, const std::string& boss, const std::uint32_t difficulty) -> void
		{
			UPrimalPlayerData* playerData = playerController->GetShooterPlayerState()->MyPlayerDataField();

			if (!playerData) return;
			static UFunction* defeatedBossFunction = playerData->FindFunctionChecked(FName("DefeatedBoss", EFindName::FNAME_Add));

			struct UFunctionParameters
			{
				AShooterCharacter* BossCharacter;
				uint32_t DifficultyIndex;
				FName TagOverride;
				AShooterPlayerController* Controller;
			};
			UFunctionParameters parameters = { nullptr, difficulty, FName(boss.c_str(), EFindName::FNAME_Add), playerController};

			playerData->ProcessEvent(defeatedBossFunction, &parameters);
			playerData->SavePlayerData(ArkApi::GetApiUtils().GetWorld());
		}
		static auto UnlockEngram(AShooterPlayerController* playerController, UClass* engram) -> void
		{
			AShooterPlayerState* playerState = playerController->GetShooterPlayerState();
			if (!playerState)
				return;
			playerState->ServerUnlockEngram(engram, false, true);

		}

		static auto ProcessEngrams(AShooterPlayerController* playerController) -> void
		{
			for (auto&& engram : unlocksCache)
				UnlockEngram(playerController, engram);
		}

		static auto ProcessBosses(AShooterPlayerController* playerController) -> void
		{
			for (const auto& boss : config.defeatedBosses)
				DefeatBoss(playerController, boss.Boss, boss.Difficulty);
		}

		static auto ProcessUnlocks(AShooterPlayerController* playerController) -> void
		{
			Log::GetLog()->info("Processing Unlocks for {}.\n", ArkApi::IApiUtils::GetSteamIdFromController(playerController));
			ProcessBosses(playerController);
			ProcessEngrams(playerController);
			SetChibi(playerController, config.chibiLevels);
		}

		static auto onStartNewShooterPlayer(AShooterGameMode* _this, APlayerController* newPlayer, bool forceCreateNewPlayerData, bool isFromLogin, const FPrimalPlayerCharacterConfigStruct& characterConfig, UPrimalPlayerData* playerData)
		{
			AShooterPlayerController* playerController = static_cast<AShooterPlayerController*>(newPlayer);
			if (!isFromLogin || playerData) //New Character or Transfer
			{
				ProcessBosses(playerController); //Eagerly Unlock bosses for new characters and transfers.
				playerController->ServerRestartPlayer();
			}
			StartNewShooterPlayer.original(_this, newPlayer, forceCreateNewPlayerData, isFromLogin, characterConfig, playerData);
			ProcessUnlocks(playerController); //Ensures PlayerData isn't destroyed.
		}

	public:
		static bool Enable()
		{
			try { config = ReadJSON("ArkApi/Plugins/ArkAutoBosses/config.json").get<Config>();  }
			catch (std::exception& e) { Log::GetLog()->error("Load Error: {}\n", e.what()); }

			StartNewShooterPlayer.reset(reinterpret_cast<decltype(StartNewShooterPlayer)::element_type*>(GetAddress("AShooterGameMode.StartNewShooterPlayer"))); //Ugly work-around to ensure internal addreses are consistent.
			ArkApi::GetHooks().SetHook("AShooterGameMode.StartNewShooterPlayer", onStartNewShooterPlayer, StartNewShooterPlayer.mut());

			API::Timer::Get().DelayExecute(CacheUnlocks, 0);
			return true;
		}
		static bool Disable()
		{
			ArkApi::GetHooks().DisableHook("AShooterGameMode.StartNewShooterPlayer", ProcessUnlocks);
			StartNewShooterPlayer.release();
			return true;
		}
	};

}