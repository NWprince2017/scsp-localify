#include <stdinclude.hpp>
#include <unordered_set>
#include <ranges>
#include <set>
#include <mutex>
#include <cstdint>
#include <cstdio>
#include <Psapi.h>
#include <intsafe.h>
#include "scgui/scGUIData.hpp"
#include <scgui/scGUIMain.hpp>
#include <mhotkey.hpp>
#include <cpprest/http_client.h>
#include <cpprest/filestream.h>
#include <boost/beast/core/detail/base64.hpp>
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/filewritestream.h>

#include "tools/unsafe.hpp"

//using namespace std;

std::function<void()> g_on_hook_ready;
std::function<void()> g_on_close;
std::function<void()> on_hotKey_0;
bool needPrintStack = false;
std::vector<std::pair<std::pair<int, int>, int>> replaceDressResIds{};
std::map<std::string, CharaParam_t> charaParam{};
CharaParam_t baseParam(0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
std::vector<std::function<bool()>> mainThreadTasks{};  // 返回 true，执行后移除列表；返�?false，执行后不移�?

std::map<int, CharaSwayStringParam_t> charaSwayStringOffset{};
std::map<int, std::string> swayTypes{
	{0x0, "Test"},
	{0x1, "HairPointed"},
	{0x2, "HairSmooth"},
	{0x3, "HairBundle"},
	{0x4, "HairFront"},
	{0x5, "ClothPointed"},
	{0x6, "ClothSmooth"},
	{0x7, "BreastPointed"},
	{0x8, "BreastSmooth"},
	{0x9, "AccessoryBundle"},
	{0xa, "AccessoryPendulum"},
	{0xb, "ClothBundle"},
	{0xc, "HairLong"},
	{0xd, "Max"},
};

std::map<int, UnitIdol> savedCostumes{};
std::map<int, UnitIdol> autoAppliedCostumes{};
UnitIdol lastSavedCostume;
UnitIdol overridenMvUnitIdols[8];
std::map<std::string, std::string> replacementTexureNames{};
std::unordered_map<Il2CppObject*, std::unique_ptr<LocalTransform>> transformOverriding{};
std::vector<std::pair<std::string, std::string>> savedTransformOverridingJson{};

std::map<int, RunwayMotion> knownRunwayMotions{};
std::vector<OverrideRunwayMotionData> overrideRunwayMotionData(5);

/// @return if the given `transform` is matched (maybe not modified if it's unavailable)
bool OverrideTransform(Il2CppObject* transform) {
	auto startSize = transformOverriding.size();
	auto it = transformOverriding.find(transform);
	if (it != transformOverriding.end()) {
		auto& localTransform = it->second;
		if (!localTransform->transform || !reflection::UnityObject_op_Implicit(localTransform->transform)) {
			transformOverriding.erase(it->first);
			return true;
		}
		else {
			localTransform->WriteLocalPosition((Il2CppObject*)transform);
			localTransform->WriteLocalRotation((Il2CppObject*)transform);
			localTransform->WriteLocalScale((Il2CppObject*)transform);
			return true;
		}
	}
	auto endSize = transformOverriding.size();
	if (endSize != startSize) {
		std::cout << "[OverrideTransform] size changed: " << startSize << " -> " << endSize << std::endl;
	}
	return false;
}
void OverrideAllTransforms() {
	auto startSize = transformOverriding.size();
	for (auto& it : transformOverriding) {
		auto& localTransform = it.second;
		if (!localTransform->transform || !reflection::UnityObject_op_Implicit(localTransform->transform)) {
			transformOverriding.erase(it.first);
		}
		else {
			localTransform->WriteLocalPosition((Il2CppObject*)localTransform->transform);
			localTransform->WriteLocalRotation((Il2CppObject*)localTransform->transform);
			localTransform->WriteLocalScale((Il2CppObject*)localTransform->transform);
		}
	}
	auto endSize = transformOverriding.size();
	if (endSize != startSize) {
		std::cout << "[OverrideAllTransforms] size changed: " << startSize << " -> " << endSize << std::endl;
	}
}


void loadGUIDataCache() {
	try {
		if (!std::filesystem::exists("scsp-gui-save.json")) return;
		std::ifstream file("scsp-gui-save.json");
		if (!file.is_open()) return;
		std::string fileContent((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		file.close();

		auto fileData = nlohmann::json::parse(fileContent);

		if (fileData.contains("charaSwayStringOffset")) {
			auto& swaySaveData = fileData["charaSwayStringOffset"];
			for (auto& sway : swaySaveData.items()) {
				const std::string keyStr = sway.key();
				const int key = atoi(keyStr.c_str());

				const auto& value = sway.value();
				const float rate = value["rate"];
				const float P_bendStrength = value["P_bendStrength"];
				const float P_baseGravity = value["P_baseGravity"];
				const float P_inertiaMoment = value["P_inertiaMoment"];
				const float P_airResistance = value["P_airResistance"];
				const float P_deformResistance = value["P_deformResistance"];

				if (auto it = charaSwayStringOffset.find(key); it != charaSwayStringOffset.end()) {
					it->second.rate = rate;
					it->second.P_bendStrength = P_bendStrength;
					it->second.P_baseGravity = P_baseGravity;
					it->second.P_inertiaMoment = P_inertiaMoment;
					it->second.P_airResistance = P_airResistance;
					it->second.P_deformResistance = P_deformResistance;
				}
				else {
					charaSwayStringOffset.emplace(key, CharaSwayStringParam_t(rate, P_bendStrength, P_baseGravity,
						P_inertiaMoment, P_airResistance, P_deformResistance));
				}
			}
		}
	}
	catch (std::exception& e) {
		printf("initcharaSwayStringOffset failed: %s\n\n", e.what());
	}
}

void initcharaSwayStringOffset() {
	for (auto& i : swayTypes) {
		charaSwayStringOffset.emplace(i.first, CharaSwayStringParam_t());
	}
	loadGUIDataCache();
}

void saveGUIDataCache() {
	try {
		auto saveData = nlohmann::json::object();
		saveData["charaSwayStringOffset"] = nlohmann::json::object();

		auto& swaySaveData = saveData["charaSwayStringOffset"];
		for (const auto& pair : charaSwayStringOffset) {
			const auto currKey = std::to_string(pair.first);
			swaySaveData[currKey] = nlohmann::json::object();
			swaySaveData[currKey]["rate"] = pair.second.rate;
			swaySaveData[currKey]["P_bendStrength"] = pair.second.P_bendStrength;
			swaySaveData[currKey]["P_baseGravity"] = pair.second.P_baseGravity;
			swaySaveData[currKey]["P_inertiaMoment"] = pair.second.P_inertiaMoment;
			swaySaveData[currKey]["P_airResistance"] = pair.second.P_airResistance;
			swaySaveData[currKey]["P_deformResistance"] = pair.second.P_deformResistance;
		}

		const auto saveStr = saveData.dump(4);
		std::ofstream out("scsp-gui-save.json");
		out << saveStr;
		out.close();
	}
	catch (std::exception& e) {
		printf("saveGUIDataCache error: %s\n", e.what());
	}
}

template<typename T, typename TF>
void convertPtrType(T* cvtTarget, TF func_ptr) {
	*cvtTarget = reinterpret_cast<T>(func_ptr);
}

#pragma region HOOK_MACRO
#ifdef __SAFETYHOOK
void AddSafetyHook(const char* orig_name, void* orig, void* hook, SafetyHookInline& result) {
	if (orig == nullptr) {
		std::cout << "[ERROR] Failed to create hook for \"" << orig_name << "\" (orig=0x" << orig << "): nullptr" << std::endl;
		return;
	}
	try {
		auto value = safetyhook::InlineHook::create(orig, hook);
		if (value) {
			result = std::move(value.value());
			std::cout << "Hook created for \"" << orig_name << "\" (orig=0x" << orig << ")" << std::endl;
		}
		else {
			auto err = value.error();
			std::cout << "[ERROR] Failed to create hook for \"" << orig_name << "\" (" << orig << "): ";
			if (err.type == safetyhook::InlineHook::Error::BAD_ALLOCATION) {
				std::cout << "BAD_ALLOCATION (" << (uint8_t)err.allocator_error << ")";
			}
			else {
				std::cout << std::to_string(err.type) << "(" << ((void*)err.ip) << ")";
			}
			std::cout << std::endl;
			return;
		}
	}
	catch (const std::exception& e) {
		std::cout << "[ERROR] Failed to create hook for \"" << orig_name << "\" (orig=0x" << orig << "): " << e.what() << std::endl;
	}
	catch (...) {
		std::cout << "[ERROR] Failed to create hook for \"" << orig_name << "\" (orig=0x" << orig << "): unknown exception" << std::endl;
	}
}
#define ADD_HOOK(_name_, _nothing_) AddSafetyHook(#_name_, (void*)_name_##_addr, (void*)_name_##_hook, _name_##_orig);
#else
#define ADD_HOOK(_name_, _fmt_) \
	auto _name_##_offset = reinterpret_cast<void*>(_name_##_addr); \
 	\
	const auto _name_##stat1 = MH_CreateHook(_name_##_offset, _name_##_hook, &_name_##_orig); \
	const auto _name_##stat2 = MH_EnableHook(_name_##_offset); \
	printf(_fmt_##" (%s, %s)\n", _name_##_offset, MH_StatusToString(_name_##stat1), MH_StatusToString(_name_##stat2))
#endif
#define ADD_HOOK_1(_name_) ADD_HOOK(_name_, #_name_##" at %p")
#define ADD_HOOK_ADDR(_raw_assembly_, _raw_namespace_, _raw_class_, _raw_method_, _val_param_count_) \
	auto _raw_class_##_##_raw_method_##_addr = il2cpp_symbols_logged::get_method_pointer(#_raw_assembly_, #_raw_namespace_, #_raw_class_, #_raw_method_, _val_param_count_); \
	ADD_HOOK_1(_raw_class_##_##_raw_method_);
#pragma endregion

bool exd = false;
HOOK_ORIG_TYPE SetResolution_orig;
HOOK_ORIG_TYPE AssembleCharacter_ApplyParam_orig;
Il2CppString* (*environment_get_stacktrace)();


void CharaParam_t::Apply() {
	const auto ApplyParamFunc = reinterpret_cast<void (*)(void*, float, float, float, float, float)>(
		HOOK_GET_ORIG(AssembleCharacter_ApplyParam)
		);
	auto currObjPtr = getObjPtr();
	if (currObjPtr) {
		ApplyParamFunc(currObjPtr, height + baseParam.height, bust + baseParam.bust,
			head + baseParam.head, arm + baseParam.arm, hand + baseParam.hand);
	}
}

void CharaParam_t::ApplyOnMainThread() {
	mainThreadTasks.push_back([this]() {
		this->Apply();
		if (!g_enable_chara_param_edit) return true;
		return !(this->gui_real_time_apply || baseParam.gui_real_time_apply);
		});
}


namespace Utils {
	template <typename KT = void*, typename VT = void*>
	class CSDictEditor {
	public:
		// @param dict: Dictionary instance.
		// @param dictTypeStr: Reflection type. eg: "System.Collections.Generic.Dictionary`2[System.Int32, System.Int32]"
		CSDictEditor(void* dict, const char* dictTypeStr) {
			dic_klass = il2cpp_symbols::get_system_class_from_reflection_type_str(dictTypeStr);
			this->dict = dict;

			get_Item_method = il2cpp_class_get_method_from_name(dic_klass, "get_Item", 1);
			Add_method = il2cpp_class_get_method_from_name(dic_klass, "Add", 2);
			ContainsKey_method = il2cpp_class_get_method_from_name(dic_klass, "ContainsKey", 1);

			dic_get_Item = (dic_get_Item_t)get_Item_method->methodPointer;
			dic_Add = (dic_Add_t)Add_method->methodPointer;
			dic_containsKey = (dic_containsKey_t)ContainsKey_method->methodPointer;
		}

		void Add(KT key, VT value) {
			dic_Add(dict, key, value, Add_method);
		}

		bool ContainsKey(KT key) {
			return dic_containsKey(dict, key, ContainsKey_method);
		}

		VT get_Item(KT key) {
			return dic_get_Item(dict, key, get_Item_method);
		}

		VT operator[] (KT key) {
			return get_Item(key);
		}

		void* dict;
		void* dic_klass;

	private:
		typedef VT(*dic_get_Item_t)(void*, KT, void* mtd);
		typedef VT(*dic_Add_t)(void*, KT, VT, void* mtd);
		typedef VT(*dic_containsKey_t)(void*, KT, void* mtd);

		CSDictEditor();
		MethodInfo* get_Item_method;
		MethodInfo* Add_method;
		MethodInfo* ContainsKey_method;
		dic_get_Item_t dic_get_Item;
		dic_Add_t dic_Add;
		dic_containsKey_t dic_containsKey;
	};

	template <typename T = void*>
	class CSListEditor {
	public:
		CSListEditor(void* list) {
			list_klass = il2cpp_symbols::get_class_from_instance(list);
			lst = list;

			lst_get_Count_method = il2cpp_class_get_method_from_name(list_klass, "get_Count", 0);
			lst_get_Item_method = il2cpp_class_get_method_from_name(list_klass, "get_Item", 1);
			lst_Add_method = il2cpp_class_get_method_from_name(list_klass, "Add", 1);

			lst_get_Count = reinterpret_cast<lst_get_Count_t>(lst_get_Count_method->methodPointer);
			lst_get_Item = reinterpret_cast<lst_get_Item_t>(lst_get_Item_method->methodPointer);
			lst_Add = reinterpret_cast<lst_Add_t>(lst_Add_method->methodPointer);
		}

		void Add(T value) {
			lst_Add(lst, value, lst_Add_method);
		}

		T get_Item(int index) {
			return lst_get_Item(lst, index, lst_get_Item_method);
		}

		int get_Count() {
			return lst_get_Count(lst, lst_get_Count_method);
		}

		T operator[] (int key) {
			return get_Item(key);
		}

		class Iterator {
		public:
			Iterator(CSListEditor<T>* editor, int index) : editor(editor), index(index) {}

			T operator*() const {
				return editor->get_Item(index);
			}

			Iterator& operator++() {
				++index;
				return *this;
			}

			bool operator!=(const Iterator& other) const {
				return index != other.index;
			}

		private:
			CSListEditor<T>* editor;
			int index;
		};

		Iterator begin() {
			return Iterator(this, 0);
		}

		Iterator end() {
			return Iterator(this, get_Count());
		}

		void* lst;
		void* list_klass;
	private:
		typedef T(*lst_get_Item_t)(void*, int, void* mtd);
		typedef void(*lst_Add_t)(void*, T, void* mtd);
		typedef int(*lst_get_Count_t)(void*, void* mtd);

		MethodInfo* lst_get_Item_method;
		MethodInfo* lst_Add_method;
		MethodInfo* lst_get_Count_method;

		lst_get_Item_t lst_get_Item;
		lst_Add_t lst_Add;
		lst_get_Count_t lst_get_Count;
	};

}


namespace
{
	void path_game_assembly();
	void patchNP(HMODULE module);
	bool mh_inited = false;
	void* load_library_w_orig = nullptr;
	bool (*CloseNPGameMon)();

	void reopen_self() {
		int result = MessageBoxW(NULL, L"未检测到目标加载，插件初始化失败。是否重启游戏？", L"提示", MB_OKCANCEL);

		if (result == IDOK)
		{
			STARTUPINFOA startupInfo{ .cb = sizeof(STARTUPINFOA) };
			PROCESS_INFORMATION pi{};
			const auto commandLine = GetCommandLineA();
			if (CreateProcessA(NULL, commandLine, NULL, NULL, FALSE, 0, NULL, NULL, &startupInfo, &pi)) {
				// printf("open external plugin: %s (%lu)\n", commandLine, pi.dwProcessId);
				// DWORD dwRetun = 0;
				// WaitForSingleObject(pi.hProcess, INFINITE);
				// GetExitCodeProcess(pi.hProcess, &dwRetun);
				// printf("plugin exit: %d\n", dwRetun);
				CloseHandle(pi.hThread);
				CloseHandle(pi.hProcess);
			}
			TerminateProcess(GetCurrentProcess(), 0);
		}
	}

	HMODULE __stdcall load_library_w_hook(const wchar_t* path)
	{
		using namespace std;
		// printf("load library: %ls\n", path);
		if (!exd && (wstring_view(path).find(L"NPGameDLL.dll") != wstring_view::npos)) {
			// printf("fing NP\n");
			// Sleep(10000000000000);
			// printf("end sleep\n");
			auto ret = reinterpret_cast<decltype(LoadLibraryW)*>(load_library_w_orig)(path);
			patchNP(ret);
			if (environment_get_stacktrace) {
				// printf("NP: %ls\n", environment_get_stacktrace()->start_char);
			}
			return ret;
		}
		// else if (path == L"GameAssembly.dll"sv) {
		//	auto ret = reinterpret_cast<decltype(LoadLibraryW)*>(load_library_w_orig)(path);
		//	return ret;
		// }
		else if (path == L"cri_ware_unity.dll"sv) {
			path_game_assembly();
		}

		return reinterpret_cast<decltype(LoadLibraryW)*>(load_library_w_orig)(path);
	}

	void* (*Encoding_get_UTF8)();
	Il2CppString* (*Encoding_GetString)(void* _this, void* bytes);
	bool isInitedEncoding = false;

	void initEncoding() {
		if (isInitedEncoding) return;
		isInitedEncoding = true;
		convertPtrType(&Encoding_get_UTF8, il2cpp_symbols::get_method_pointer("mscorlib.dll", "System.Text", "Encoding", "get_UTF8", 0));
		convertPtrType(&Encoding_GetString, il2cpp_symbols::get_method_pointer("mscorlib.dll", "System.Text", "Encoding", "GetString", 1) - 0x1B0);
		// convertPtrType(&Encoding_GetString, 0x182ABF800);  // F800 = PTR - 0x1B0
		// printf("Encoding_GetString at %p\n", Encoding_GetString);
		/*
		auto Encoding_GetString_addr = il2cpp_symbols::find_method("mscorlib.dll", "System.Text", "Encoding", [](const MethodInfo* mtd) {
			if (mtd->parameters_count == 1) {
				//if (strcmp(mtd->name, "GetString") == 0) {
				//	printf("GetString Type: %x\n", mtd->parameters->parameter_type->type);
				//}
			}
			return false;
			});
			*/

	}

	Il2CppString* bytesToIl2cppString(void* bytes) {
		initEncoding();
		auto encodingInstance = Encoding_get_UTF8();
		// return Encoding_GetString(encodingInstance, bytes);
		return Encoding_GetString(encodingInstance, bytes);
	}

	struct TransparentStringHash : std::hash<std::wstring>, std::hash<std::wstring_view>
	{
		using is_transparent = void;
	};

	typedef std::unordered_set<std::wstring, TransparentStringHash, std::equal_to<void>> AssetPathsType;
	std::unordered_map<std::string, void*> CustomAssetBundleGcHandleMap{};
	void* (*AssetBundle_LoadFromFile)(Il2CppString* path, UINT32 crc, UINT64 offset);

	void* ReplaceFontGcHandle;
	bool (*Object_IsNativeObjectAlive)(void*);
	void* (*AssetBundle_LoadAsset)(void* _this, Il2CppString* name, Il2CppReflectionType* type);
	Il2CppReflectionType* Font_Type;

	std::unordered_set<std::string> unfoundAssetBundles{};

	void* LoadExternAsset(std::string assetFullPath, Il2CppReflectionType* assetType) {
		if (unfoundAssetBundles.find(assetFullPath) != unfoundAssetBundles.end()) {
			return nullptr;
		}

		const std::string delimiter = "::";
		size_t index = assetFullPath.rfind(delimiter);
		std::string bundlePath, assetPath;
		if (index == std::string::npos) {
			bundlePath = "";
			assetPath = assetFullPath;
		}
		else {
			bundlePath = assetFullPath.substr(0, index);
			assetPath = assetFullPath.substr(index + delimiter.length());
		}

		if (bundlePath.empty() || !std::filesystem::exists(bundlePath)) {
			unfoundAssetBundles.emplace(assetFullPath);
			std::cout << "[ERROR] AssetBundle \"" << bundlePath << "\" doesn't exist." << std::endl;
			return nullptr;
		}

		void* assetBundle;
		auto it = CustomAssetBundleGcHandleMap.find(bundlePath);
		if (it != CustomAssetBundleGcHandleMap.end()) {
			auto gcHandle = it->second;
			assetBundle = il2cpp_gchandle_get_target(gcHandle);
		}
		else
		{
			static auto func_Path_GetFullPath = reinterpret_cast<Il2CppString * (*)(Il2CppString*)>(il2cpp_symbols_logged::get_method_pointer("mscorlib.dll", "System.IO", "Path", "GetFullPath", 1));
			const auto extraAssetBundle = AssetBundle_LoadFromFile(func_Path_GetFullPath(il2cpp_string_new(bundlePath.c_str())), 0, 0);
			if (extraAssetBundle) {
				assetBundle = extraAssetBundle;
				CustomAssetBundleGcHandleMap.emplace(bundlePath, il2cpp_gchandle_new(extraAssetBundle, false));
			}
			else {
				std::cout << "[ERROR] Failed to load AssetBundle \"" << bundlePath << "\"." << std::endl;
				return nullptr;
			}
		}

		auto asset = AssetBundle_LoadAsset(assetBundle, il2cpp_string_new(assetPath.c_str()), assetType);
		if (!asset) {
			std::cout << "[ERROR] Failed to load asset \"" << assetFullPath << "\"." << std::endl;
			return nullptr;
		}
		return asset;
	}

	bool firstFontUnfoundError = true;
	void* getReplaceFont() {
		void* replaceFont{};
		if (g_custom_font_path.empty()) return replaceFont;

		if (ReplaceFontGcHandle)
		{
			replaceFont = il2cpp_gchandle_get_target(ReplaceFontGcHandle);

			// 加载场景时会�?Resources.UnloadUnusedAssets 干掉，且不受 DontDestroyOnLoad 影响，暂且判断是否存活，并在必要的时候重新加�?
			// TODO: 考虑挂载�?GameObject �?
			// AssetBundle 不会被干�?
			if (Object_IsNativeObjectAlive(replaceFont))
			{
				return replaceFont;
			}
			else
			{
				il2cpp_gchandle_free(std::exchange(ReplaceFontGcHandle, nullptr));
			}
		}

		replaceFont = LoadExternAsset(g_custom_font_path, Font_Type);
		if (replaceFont)
		{
			ReplaceFontGcHandle = il2cpp_gchandle_new(replaceFont, false);
		}
		else
		{
			if (firstFontUnfoundError) {
				firstFontUnfoundError = false;
				printf("[ERROR] Failed to load the font to replace.\n");
			}
		}

		return replaceFont;
	}

	HOOK_ORIG_TYPE DataFile_GetBytes_orig;
	bool (*DataFile_IsKeyExist)(Il2CppString*);

	void set_fps_hook(int value);
	void set_vsync_count_hook(int value);

	std::filesystem::path dumpBasePath("dumps");

	// 调用之前检�?DataFile_IsKeyExist
	void fmtAndDumpJsonBytesData(const std::wstring& dumpName) {
		const auto dumpNameIl = il2cpp_symbols::NewWStr(dumpName);
		auto dataBytes = (reinterpret_cast<void* (*)(Il2CppString*)>HOOK_GET_ORIG(DataFile_GetBytes))(dumpNameIl);
		auto newStr = bytesToIl2cppString(dataBytes);
		auto writeWstr = std::wstring(newStr->start_char);
		const std::wstring searchStr = L"\r";
		const std::wstring replaceStr = L"\\r";
		size_t pos = writeWstr.find(searchStr);
		while (pos != std::wstring::npos) {
			writeWstr.replace(pos, 1, replaceStr);
			pos = writeWstr.find(searchStr, pos + replaceStr.length());
		}
		if (writeWstr.ends_with(L",]")) {  // 代哥�?Json 就是不一�?
			writeWstr.erase(writeWstr.length() - 2, 1);
		}
		const auto dumpLocalFilePath = dumpBasePath / SCLocal::getFilePathByName(dumpName, true, dumpBasePath);
		std::ofstream dumpFile(dumpLocalFilePath, std::ofstream::out);
		dumpFile << utility::conversions::to_utf8string(writeWstr).c_str();
		printf("dump %ls success. (%ls)\n", dumpName.c_str(), dumpLocalFilePath.c_str());
	}

	int dumpScenarioDataById(const std::string& sid) {
		int i = 0;
		int notFoundCount = 0;
		int dumpedCount = 0;
		printf("dumping: %s\n", sid.c_str());
		while (true) {
			const auto dumpName = std::format(L"{}_{:02}.json", utility::conversions::to_utf16string(sid), i);
			const auto dumpNameIl = il2cpp_symbols::NewWStr(dumpName);
			if (!DataFile_IsKeyExist(dumpNameIl)) {
				// printf("%ls - continue.\n", dumpName.c_str());
				notFoundCount++;
				if (notFoundCount < 5) {
					i++;
					continue;
				}
				else {
					break;
				}
			}
			fmtAndDumpJsonBytesData(dumpName);
			dumpedCount++;
			i++;
		}
		return dumpedCount;
	}

	bool dumpScenarioDataByJsonFileName(const std::string& fileName) {
		std::string searchName;
		if (!fileName.ends_with(".json")) {
			searchName = fileName + ".json";
		}
		else {
			searchName = fileName;
		}
		auto searchNameStr = il2cpp_string_new(searchName.c_str());
		if (!DataFile_IsKeyExist(searchNameStr)) return false;
		fmtAndDumpJsonBytesData(utility::conversions::to_string_t(searchName));
		return true;
	}

	void dumpByteArray(const std::wstring& tag, const std::wstring& name, void* bytes) {
		static auto WriteAllBytes = reinterpret_cast<void (*)(Il2CppString*, void*)>(
			il2cpp_symbols::get_method_pointer("mscorlib.dll", "System.IO",
				"File", "WriteAllBytes", 2)
			);

		std::filesystem::path dumpPath = dumpBasePath / tag;
		if (!std::filesystem::is_directory(dumpPath)) {
			std::filesystem::create_directories(dumpPath);
		}
		std::filesystem::path dumpName = dumpPath / name;
		WriteAllBytes(il2cpp_symbols::NewWStr(dumpName.c_str()), bytes);

	}

	int dumpScenarioFromCatalog();

	std::string localizationDataCache("{}");
	int dumpAllScenarioData() {
		try {
			return dumpScenarioFromCatalog();
		}
		catch (std::exception& e) {
			printf("dumpScenarioFromCatalog error: %s\n", e.what());
		}
		// 下方方法已过�?
		int totalCount = 0;
		const auto titleData = nlohmann::json::parse(localizationDataCache);
		for (auto& it : titleData.items()) {
			const auto& key = it.key();
			const std::string_view keyView(key);
			if (keyView.starts_with("mlADVInfo_Title")) {
				totalCount += dumpScenarioDataById(key.substr(16));
			}
		}
		return totalCount;
	}

	bool guiStarting = false;
	void startSCGUI() {
		if (guiStarting) return;
		guiStarting = true;
		std::thread([]() {
			printf("GUI START\n");
			guimain();
			guiStarting = false;
			printf("GUI END\n");
			}).detach();
	}

	void* (*ReadAllBytes)(Il2CppString*);
	void* readFileAllBytes(const std::wstring& name) {
		return ReadAllBytes(il2cpp_symbols::NewWStr(name));
	}

	void itLocalizationManagerDic(void* _this) {
		if (!SCGUIData::needExtractText) return;
		SCGUIData::needExtractText = false;

		static auto LocalizationManager_klass = il2cpp_symbols::get_class_from_instance(_this);
		static auto dic_field = il2cpp_class_get_field_from_name(LocalizationManager_klass, "dic");
		auto dic = il2cpp_symbols::read_field(_this, dic_field);

		static auto toJsonStr = reinterpret_cast<Il2CppString * (*)(void*)>(
			il2cpp_symbols::get_method_pointer("Newtonsoft.Json.dll", "Newtonsoft.Json",
				"JsonConvert", "SerializeObject", 1)
			);

		auto jsonStr = toJsonStr(dic);
		printf("dump text...\n");
		std::wstring jsonWStr(jsonStr->start_char);

		if (!std::filesystem::is_directory(dumpBasePath)) {
			std::filesystem::create_directories(dumpBasePath);
		}
		std::ofstream file(dumpBasePath / "localify.json", std::ofstream::out);
		const auto writeStr = utility::conversions::to_utf8string(jsonWStr);
		localizationDataCache = writeStr;
		file << writeStr.c_str();
		file.close();
		printf("write done.\n");

		const auto dumpCount = dumpAllScenarioData();
		printf("Dump finished. Total %d files\n", dumpCount);

		return;
	}

	void updateDicText(void* _this, Il2CppString* category, int id, std::string& newStr) {
		static auto LocalizationManager_klass = il2cpp_symbols::get_class_from_instance(_this);
		static auto dic_field = il2cpp_class_get_field_from_name(LocalizationManager_klass, "dic");
		auto dic = il2cpp_symbols::read_field(_this, dic_field);

		static auto dic_klass = il2cpp_symbols::get_class_from_instance(dic);
		static auto dict_get_Item = reinterpret_cast<void* (*)(void*, void*)>(
			il2cpp_class_get_method_from_name(dic_klass, "get_Item", 1)->methodPointer
			);

		auto categoryDict = dict_get_Item(dic, category);
		static auto cdic_klass = il2cpp_symbols::get_class_from_instance(categoryDict);
		static auto dict_set_Item = reinterpret_cast<void* (*)(void*, int, void*)>(
			il2cpp_class_get_method_from_name(cdic_klass, "set_Item", 2)->methodPointer
			);
		dict_set_Item(categoryDict, id, il2cpp_string_new(newStr.c_str()));

	}

	HOOK_ORIG_TYPE StoryExtensions_IsLocked_orig;
	bool StoryExtensions_IsLocked_hook(void* self) {
		if (g_unlock_PIdol_and_SChara_events) {
			return false;
		}
		else {
			return HOOK_CAST_CALL(bool, StoryExtensions_IsLocked)(self);
		}
	}

	// photo studio unlocking
	// =====================================================================
	// [PATCH] v10 - permanent limited-time EVENTS (Shiny Runway and friends)
	//
	//   The client decides "this event is over" from its own master table
	//   PRISM.Definitions.MstEvent { Id, EventType, BeginDate, EndDate, ... }.
	//   That table is inside the encrypted master, so no offline-server reply
	//   can change it - the two DateTime fields have to be rewritten in
	//   memory, exactly like the costume isUnlocked_ flag was.
	//
	//   v10 hooked set_BeginDate / set_EndDate and WROTE the new dates back into
	//   the master rows.  That crashed the game before the title screen with
	//   ErrorCode[E6A359E6] and produced a 17 MB mst-events.txt full of garbage,
	//   i.e. either the resolved method or its calling convention was wrong, so
	//   the writes landed on unrelated objects.
	//
	//   v11 is therefore strictly READ-ONLY: it hooks the *getter* get_EndDate
	//   and replaces its RETURN VALUE.  Nothing is ever written to memory, and
	//   the override only happens when the returned value is unmistakably a
	//   DateTime in ticks (2000..2100).  If this build ever returns something
	//   else (e.g. a pointer to the struct), the original value is handed back
	//   untouched - a wrong guess can therefore only fail to have an effect, it
	//   cannot corrupt anything.
	//
	//   System.DateTime is one ulong of 100 ns ticks since 0001-01-01:
	//       ticks = (unixSeconds + 62135596800) * 10000000
	//   2000-01-01 -> 630822816000000000
	//   2038-01-19 -> 642830804470000000   (the value handed back = "never ends")
	//
	//   mst-events.txt always records the real values, so the data is verifiable
	//   even with the patch switched off (scsp-config.json "extendEventDates").
	//
	//   v13 (diagnostic, NO hooks): v12 crashed the game twice right after it
	//   installed its hooks (0xc0000005 inside GameAssembly, same offset both
	//   runs).  runway-diag.txt showed why the getters cannot be hooked:
	//   get_EndDate / get_BeginDate report MH_ERROR_ALREADY_CREATED (their code
	//   addresses are already hooked or shared via ICF with unrelated methods -
	//   which is also why v11 never produced mst-events.txt), and the two new
	//   hooks that did install sit on addresses shared by other getters.
	//   v13 therefore installs NOTHING and calls NOTHING: it reads the real
	//   stored values straight out of MasterData.Events by field offset
	//   (mst-events-table.txt) and writes an install report (runway-diag.txt).
	//
	//   v14 (the real fix, still NO hooks): once per session the newest
	//   Shiny Runway row (EventType==7, highest Id) is overwritten in the
	//   live master - EndDate and DisplayEndDate := 2099-12-31 23:59:59,
	//   BeginDate stays 2026-09-26 20:30 (already in the past), so the
	//   event window permanently contains "now" and the event can never be
	//   over.  The write targets the exact FieldInfo offset of the row that
	//   was just verified to be a real MstEvent - no hook, no managed call,
	//   no other object is touched.  The result is logged to
	//   mst-events-patch.txt (old value -> new value, read back).
	//
	//   v15: identical writes, but applied to EVERY Shiny Runway row
	//   (EventType==7) of this master - all five holdings become permanent
	//   events in the list, each still with its own real BeginDate.  Rows
	//   that fail the DateTime sanity check are skipped individually, so
	//   one odd row can never keep the others from being patched.
	// =====================================================================

	namespace EventDates {
		// v13: strictly read-only master dump.  No hooks, no managed calls -
		// every value is read straight out of the master objects by field
		// offset, so nothing here can execute game code or write memory.
		std::mutex& Mtx() { static std::mutex m; return m; }

		void DumpLine(const std::string& line) {
			std::lock_guard<std::mutex> lk(Mtx());
			std::ofstream f("mst-events-table.txt", std::ios::app);
			if (!f.is_open()) return;
			f << line << "\n";
			f.flush();
		}

		bool PlausiblePtr(uint64_t p) {
			return p >= 0x10000ULL && p < 0x7FFFFFFFFFFFULL && (p & 7) == 0;
		}

		// klass pointer of a live object = first 8 bytes (pure read)
		void* ClassOf(void* obj) {
			if (!obj || !PlausiblePtr((uint64_t)obj)) return nullptr;
			return il2cpp_symbols::get_class_from_instance(obj);
		}

		const char* ClassNameOf(void* obj) {
			auto k = ClassOf(obj);
			return k ? il2cpp_class_get_name(k) : nullptr;
		}

		FieldInfo* Field(void* klass, const char* name) {
			return klass ? il2cpp_symbols_logged::il2cpp_class_get_field_from_name(klass, name) : nullptr;
		}

		void* ReadPtr(void* obj, const char* fieldName) {
			auto fi = Field(ClassOf(obj), fieldName);
			if (!fi) return nullptr;
			return il2cpp_symbols::read_field<void*>(obj, fi);
		}

		int ReadInt(void* obj, const char* fieldName) {
			auto fi = Field(ClassOf(obj), fieldName);
			if (!fi) return -1;
			return il2cpp_symbols::read_field<int>(obj, fi);
		}

		std::string Hex(uint64_t v) {
			char b[32];
			snprintf(b, sizeof(b), "%llx", (unsigned long long)v);
			return b;
		}

		std::string PtrStr(void* p) { return "0x" + Hex((uint64_t)p); }

		// IL2CPP array on x64: [klass(8) monitor(8) bounds(8) max_length(4)+pad]
		// -> elements start at +0x20
		int ArrayLength(void* arr) {
			if (!arr || !PlausiblePtr((uint64_t)arr)) return 0;
			int n = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(arr) + 0x18);
			return (n >= 0 && n <= 1000000) ? n : 0;
		}
		uintptr_t ArrayElem(void* arr, int i, int stride) {
			return reinterpret_cast<uintptr_t>(arr) + 0x20 + (size_t)i * stride;
		}

		// MstEvent backing fields (names verified against global-metadata.dat)
		struct MstFields {
			FieldInfo* id = nullptr;
			FieldInfo* type = nullptr;
			FieldInfo* begin = nullptr;
			FieldInfo* end = nullptr;
			FieldInfo* dispEnd = nullptr;
		};
		MstFields& Fields() {
			static MstFields f = [] {
				MstFields r;
				auto k = il2cpp_symbols_logged::get_class("PRISM.Definitions.dll", "PRISM.Definitions", "MstEvent");
				if (k) {
					r.id = Field(k, "<Id>k__BackingField");
					r.type = Field(k, "<EventType>k__BackingField");
					r.begin = Field(k, "<BeginDate>k__BackingField");
					r.end = Field(k, "<EndDate>k__BackingField");
					r.dispEnd = Field(k, "<DisplayEndDate>k__BackingField");
				}
				return r;
			}();
			return f;
		}

		// ---------------- v14 "never ends" patch (field write only) ----------------
		// 2099-12-31 23:59:59 UTC as a System.DateTime: ticks since 0001-01-01
		// = 662380415990000000, Kind=Utc sets bit 62.
		constexpr uint64_t kNeverEnds = 5274066434417387904ULL;

		// v15: every EventType==7 row seen by the master walk (dedup by Id -
		// the walk can be attempted more than once per session)
		std::vector<std::pair<int, void*>> g_runwayRows;

		void PatchLine(const std::string& line) {
			std::lock_guard<std::mutex> lk(Mtx());
			std::ofstream f("mst-events-patch.txt", std::ios::app);
			if (!f.is_open()) return;
			f << line << "\n";
			f.flush();
		}

		bool LooksLikeDateTime(uint64_t raw) {
			uint64_t ticks = raw & 0x3FFFFFFFFFFFFFFFULL;
			return ticks >= 630822816000000000ULL   // 2000-01-01
				&& ticks <= 662380415990000000ULL;  // 2099-12-31
		}

		// called once, right after the master walk has seen every row
		void ApplyPatch() {
			static bool done = false;
			if (done) return;
			done = true;
			if (!g_extend_event_dates) {
				PatchLine("extendEventDates=false - master untouched (diagnostic run)");
				return;
			}
			if (g_runwayRows.empty()) {
				PatchLine("no EventType==7 row found - master untouched");
				return;
			}
			auto& F = Fields();
			if (!F.end || !F.dispEnd) {
				PatchLine("MstEvent date fields unresolved - master untouched");
				return;
			}
			PatchLine("patching " + std::to_string(g_runwayRows.size()) + " Shiny Runway row(s)");
			for (auto& r : g_runwayRows) {
				int id = r.first;
				void* row = r.second;
				uint64_t oldEnd = il2cpp_symbols::read_field<uint64_t>(row, F.end);
				uint64_t oldDisp = il2cpp_symbols::read_field<uint64_t>(row, F.dispEnd);
				// sanity: the old values must look like real DateTimes (2000..2099),
				// otherwise this row is not what we think it is - do not touch it
				if (!LooksLikeDateTime(oldEnd) || !LooksLikeDateTime(oldDisp)) {
					PatchLine("row id=" + std::to_string(id)
						+ " has unexpected date values - skipped");
					continue;
				}
				il2cpp_symbols::write_field<uint64_t>(row, F.end, kNeverEnds);
				il2cpp_symbols::write_field<uint64_t>(row, F.dispEnd, kNeverEnds);
				uint64_t newEnd = il2cpp_symbols::read_field<uint64_t>(row, F.end);
				uint64_t newDisp = il2cpp_symbols::read_field<uint64_t>(row, F.dispEnd);
				PatchLine("PATCHED id=" + std::to_string(id)
					+ " row=" + PtrStr(row)
					+ " end " + std::to_string(oldEnd) + " -> " + std::to_string(newEnd)
					+ " | displayEnd " + std::to_string(oldDisp) + " -> " + std::to_string(newDisp));
			}
		}

		// (v13: no call logging, no caller resolution, no managed method calls -
		// all values are read straight from the row's fields.)

		// ---------------- v13 master table dump (pure field reads) ----------------
		// MasterData.Events (name verified against global-metadata.dat) holds the
		// MstEvent table.  Dump the client's own rows once per session so the real
		// stored dates can be compared with what the getters report.  Every value
		// below is read straight from the object graph - no method is called.
		std::string CN(void* o) {
			const char* n = ClassNameOf(o);
			return n ? n : "?";
		}

		void DumpRow(void* item, int* count) {
			if (!item || *count >= 4000) return;
			if (!PlausiblePtr((uint64_t)item)) return;
			auto k = il2cpp_symbols::get_class_from_instance(item);
			const char* cn = k ? il2cpp_class_get_name(k) : nullptr;
			if (!cn || strcmp(cn, "MstEvent") != 0) return;
			auto& F = Fields();
			if (!F.id || !F.type || !F.begin || !F.end || !F.dispEnd) return;
			++*count;
			int idv = il2cpp_symbols::read_field<int>(item, F.id);
			int tv = il2cpp_symbols::read_field<int>(item, F.type);
			if (tv == 7) {
				// v15 target list: dedup by Id, keep the first pointer seen
				bool seen = false;
				for (auto& r : g_runwayRows) { if (r.first == idv) { seen = true; break; } }
				if (!seen && g_runwayRows.size() < 64) g_runwayRows.push_back(std::make_pair(idv, item));
			}
			char buf[320];
			snprintf(buf, sizeof(buf),
				"MstEvent id=%d type=%d begin=%llu end=%llu displayEnd=%llu",
				idv, tv,
				(unsigned long long)il2cpp_symbols::read_field<uint64_t>(item, F.begin),
				(unsigned long long)il2cpp_symbols::read_field<uint64_t>(item, F.end),
				(unsigned long long)il2cpp_symbols::read_field<uint64_t>(item, F.dispEnd));
			DumpLine(buf);
		}

		bool Contains(const char* hay, const char* needle) {
			return hay && strstr(hay, needle) != nullptr;
		}

		// walkers: the container class is decided from its own name, then the
		// backing fields are read directly (List`1 / Dictionary`2 / SortedList`2 /
		// array).  Element stride assumes the standard IL2CPP x64 layout.
		void DumpListLike(void* list, int* n, const char* itemsName, const char* sizeName) {
			auto arr = ReadPtr(list, itemsName);
			int size = ReadInt(list, sizeName);
			if (size < 0) size = ArrayLength(arr);
			if (size > 4000) size = 4000;
			DumpLine("  (list " + CN(list) + " size=" + std::to_string(size) + ")");
			if (!arr || !PlausiblePtr((uint64_t)arr)) { DumpLine("  (items array missing)"); return; }
			for (int i = 0; i < size; i++) DumpRow(*(void**)ArrayElem(arr, i, 8), n);
		}

		void DumpDictionary(void* dict, int* n) {
			auto entries = ReadPtr(dict, "_entries");
			int count = ReadInt(dict, "_count");
			if (count < 0) count = ArrayLength(entries);
			if (count > 1000000) count = 1000000;
			DumpLine("  (dict " + CN(dict) + " count=" + std::to_string(count) + ")");
			if (!entries || !PlausiblePtr((uint64_t)entries)) { DumpLine("  (entries array missing)"); return; }
			for (int i = 0; i < count; i++) {
				uintptr_t e = ArrayElem(entries, i, 24);   // Entry<int, MstEvent>
				if (*(int*)e < 0) continue;               // free slot
				DumpRow(*(void**)(e + 16), n);
			}
		}

		void DumpSortedList(void* sl, int* n) {
			auto values = ReadPtr(sl, "values");
			int size = ReadInt(sl, "_size");
			if (size < 0) size = ArrayLength(values);
			if (size > 4000) size = 4000;
			DumpLine("  (sortedlist " + CN(sl) + " size=" + std::to_string(size) + ")");
			if (!values || !PlausiblePtr((uint64_t)values)) { DumpLine("  (values array missing)"); return; }
			for (int i = 0; i < size; i++) DumpRow(*(void**)ArrayElem(values, i, 8), n);
		}

		void DumpArray(void* arr, int* n) {
			int len = ArrayLength(arr);
			DumpLine("  (array len=" + std::to_string(len) + ")");
			for (int i = 0; i < len && i < 4000; i++) DumpRow(*(void**)ArrayElem(arr, i, 8), n);
		}

		// fallback: storage class unknown - probe the usual backing field names
		// and report what exists so the next build can target it exactly
		void ProbeUnknown(void* container, int* n) {
			(void)n;
			auto k = ClassOf(container);
			DumpLine("  (unknown container " + CN(container) + " - probing fields)");
			static const char* kProbe[] = { "_items", "items", "_values", "values", "_list", "list", "_entries", "entries", "_array", "array", "_data", "data" };
			for (const char* fn : kProbe) {
				auto fi = Field(k, fn);
				if (!fi) continue;
				DumpLine(std::string("    field ") + fn + " offset=" + std::to_string(fi->offset));
			}
		}

		void TryDumpTable() {
			static long frame = 0;
			static int tries = 0;
			static bool finished = false;
			if (finished) return;
			if ((++frame % 60) != 1) return;
			if (++tries > 180) { finished = true; DumpLine("(gave up after 180 attempts)"); return; }

			// the only managed calls kept: two getters proven safe in v11/v12
			static auto method_get_Instance = il2cpp_symbols_logged::get_method(
				"PRISM.Adapters.dll", "PRISM.Adapters", "IAdapterLocator", "get_Instance", 0);
			if (!method_get_Instance || !method_get_Instance->methodPointer) return;
			auto locator = method_get_Instance->Invoke<Il2CppObject*>(nullptr, {});
			if (!locator) return;
			auto locatorType = il2cpp_symbols::get_class_from_instance(locator);
			auto mget = locatorType ? il2cpp_class_get_method_from_name(locatorType, "get_MasterData", 0) : nullptr;
			if (!mget || !mget->methodPointer) return;
			auto masterData = mget->Invoke<Il2CppObject*>(locator, {});
			if (!masterData) return;

			static auto klass_MasterData = il2cpp_symbols_logged::get_class(
				"PRISM.Definitions.dll", "PRISM.Definitions", "MasterData");
			if (!klass_MasterData) return;
			auto fi = il2cpp_symbols_logged::il2cpp_class_get_field_from_name(klass_MasterData, "Events");
			if (!fi) { DumpLine("MasterData.Events field not found"); return; }
			auto table = il2cpp_symbols::read_field<Il2CppObject*>(masterData, fi);
			if (!table) { DumpLine("MasterData.Events = null"); return; }
			DumpLine("MasterData.Events -> " + CN(table) + " @ " + PtrStr(table));

			// SortedMasterTable`2 itself has no fields - "data" lives on the base
			// MasterTable`1, which il2cpp_class_get_field_from_name also searches
			auto data = ReadPtr(table, "data");
			DumpLine("  data=" + PtrStr(data));
			if (!data || !PlausiblePtr((uint64_t)data)) return;
			const char* dcn = ClassNameOf(data);
			DumpLine(std::string("  data class=") + (dcn ? dcn : "?"));
			int n = 0;
			if (Contains(dcn, "List`1")) DumpListLike(data, &n, "_items", "_size");
			else if (Contains(dcn, "Dictionary`2")) DumpDictionary(data, &n);
			else if (Contains(dcn, "SortedList`2")) DumpSortedList(data, &n);
			else if (Contains(dcn, "[]")) DumpArray(data, &n);
			else ProbeUnknown(data, &n);
			DumpLine("  entries=" + std::to_string(n));
			if (n > 0) {
				finished = true;
				ApplyPatch();   // v15: overwrite every Shiny Runway row's dates
			}
		}
	}

	// =====================================================================
	// [DIAG] v21 - runway master/runtime field dump (read-only reflection).
	//   The il2cpp_symbols import table has no field-enumeration entries, so
	//   il2cpp_class_get_fields / il2cpp_field_get_flags are resolved straight
	//   from GameAssembly.dll; FieldInfo itself carries name/type/offset.
	//   Output (append):
	//     runway-master-dump.txt  - MstRunway*/MstEvent class field lists,
	//       the MasterData field list, every Runway* master table row and
	//       every Shiny Runway MstEvent row with ALL of its fields.
	//     runway-runtime-dump.txt - every RunwayEventStartData handed to
	//       PreLoadAsync (once per object).
	//   Read-only: no writes, no managed calls beyond the two getters
	//   EventDates already uses.
	// =====================================================================
	namespace RunwayDump {
		using EventDates::PlausiblePtr;
		using EventDates::ClassOf;
		using EventDates::ClassNameOf;
		using EventDates::ArrayLength;
		using EventDates::ArrayElem;
		using EventDates::PtrStr;
		using EventDates::Hex;
		using EventDates::ReadPtr;
		using EventDates::ReadInt;

		int g_lines = 0;
		std::unordered_set<uintptr_t> g_seen;
		int g_startDataClasses = 0;

		void DumpLine(const char* file, const std::string& line) {
			if (g_lines++ > 45000) return;
			std::lock_guard<std::mutex> lk(EventDates::Mtx());
			std::ofstream f(file, std::ios::app);
			if (!f.is_open()) return;
			f << line << "\n";
			f.flush();
		}
		void ToMaster(const std::string& l) { DumpLine("runway-master-dump.txt", l); }
		void ToRuntime(const std::string& l) { DumpLine("runway-runtime-dump.txt", l); }

		struct FieldApi {
			void* (*class_get_fields)(void*, void**) = nullptr;
			uint32_t(*field_get_flags)(void*) = nullptr;
			bool ok = false;
		};
		FieldApi& API() {
			static FieldApi a = [] {
				FieldApi r;
				auto m = GetModuleHandleW(L"GameAssembly.dll");
				if (m) {
					r.class_get_fields = reinterpret_cast<decltype(r.class_get_fields)>(GetProcAddress(m, "il2cpp_class_get_fields"));
					r.field_get_flags = reinterpret_cast<decltype(r.field_get_flags)>(GetProcAddress(m, "il2cpp_field_get_flags"));
				}
				r.ok = r.class_get_fields && r.field_get_flags;
				return r;
			}();
			return a;
		}

		bool NameHas(const char* n, std::initializer_list<const char*> keys) {
			if (!n) return false;
			for (auto k : keys) if (strstr(n, k)) return true;
			return false;
		}

		// best-effort "Id" of an Mst* row (backing field then property name)
		std::string BestId(void* obj, const char* cn) {
			if (!obj || !cn || strncmp(cn, "Mst", 3) != 0) return "";
			auto k = ClassOf(obj);
			for (const char* fname : { "<Id>k__BackingField", "<Id>", "Id" }) {
				auto fi = il2cpp_symbols_logged::il2cpp_class_get_field_from_name(k, fname);
				if (fi) return std::format(" {}#{}", cn, il2cpp_symbols::read_field<int>(obj, fi));
			}
			return "";
		}

		std::string ReadValue(void* obj, const FieldInfo* fi, const std::string& ts) {
			const size_t off = (size_t)fi->offset;
			if (off < 0x10 || off > 0x8000) return "(bad-off)";
			auto p = reinterpret_cast<uintptr_t>(obj) + off;
			uint64_t raw = *reinterpret_cast<uint64_t*>(p);
			if (ts == "System.String") {
				try {
					void* s = il2cpp_field_get_value_object((void*)fi, obj);
					if (!s) return "null";
					return "\"" + reinterpret_cast<Il2CppString*>(s)->ToUtf8String() + "\"";
				}
				catch (...) { return "(string-err)"; }
			}
			if (ts == "System.Int64" || ts == "System.UInt64" || ts == "System.DateTime")
				return std::format("{} 0x{}", (long long)raw, Hex(raw));
			if (ts == "System.Int32" || ts == "System.UInt32")
				return std::to_string((int)(uint32_t)raw);
			if (ts == "System.Boolean")
				return (raw & 0xFF) ? "true" : "false";
			if (ts == "System.Single") {
				uint32_t u = (uint32_t)raw; float f = 0;
				std::memcpy(&f, &u, 4);
				return std::to_string(f);
			}
			if (ts == "System.Double") {
				double d = 0; std::memcpy(&d, &raw, 8);
				return std::to_string(d);
			}
			if (PlausiblePtr(raw)) {
				std::string r = PtrStr(reinterpret_cast<void*>(raw));
				auto k2 = ClassOf(reinterpret_cast<void*>(raw));
				const char* cn2 = k2 ? il2cpp_class_get_name(k2) : nullptr;
				if (cn2) r += std::string(" ") + cn2 + BestId(reinterpret_cast<void*>(raw), cn2);
				return r;
			}
			return "0x" + Hex(raw);
		}

		void ReflectDump(void* obj, int depth, const char* tag, bool runtime);

		void DumpContainerElems(void* cont, int stride, int capacity, const char* tag, int depth, bool runtime) {
			int len = ArrayLength(cont);
			if (len > capacity) len = capacity;
			for (int i = 0; i < len; i++) {
				auto e = ArrayElem(cont, i, stride);
				if (!e) continue;
				void* item = *reinterpret_cast<void**>(e);
				if (!item) continue;
				ReflectDump(item, depth, tag, runtime);
			}
		}

		void ReflectDump(void* obj, int depth, const char* tag, bool runtime) {
			if (!obj || !PlausiblePtr((uint64_t)obj)) return;
			void (*W)(const std::string&) = runtime ? ToRuntime : ToMaster;
			if (!g_seen.insert((uintptr_t)obj).second) return;
			auto k = ClassOf(obj);
			const char* cn = k ? il2cpp_class_get_name(k) : nullptr;
			std::string c = cn ? cn : "?";
			W(std::format("  [{}] {} @ {}", tag, c, PtrStr(obj)));

			if (c.find("List`1") != std::string::npos) {
				auto arr = ReadPtr(obj, "_items");
				int size = ReadInt(obj, "_size");
				W(std::format("    list size={} items={}", size, PtrStr(arr)));
				if (arr && PlausiblePtr((uint64_t)arr)) DumpContainerElems(arr, 8, 64, tag, depth, runtime);
				return;
			}
			if (!c.empty() && c.back() == ']') {
				if (NameHas(c.c_str(), { "Runway", "Scene", "Stage", "Mst" })) DumpContainerElems(obj, 8, 64, tag, depth, runtime);
				return;
			}
			if (!API().ok) { W("    (field API unavailable)"); return; }

			void* iter = nullptr;
			void* fiRaw = nullptr;
			int found = 0;
			while ((fiRaw = API().class_get_fields(k, &iter)) != nullptr) {
				if (++found > 250) { W("    ... (field cap)"); break; }
				auto fi = reinterpret_cast<const FieldInfo*>(fiRaw);
				uint32_t flags = API().field_get_flags(fiRaw);
				if (flags & (0x10 | 0x40)) continue;   // static / literal
				std::string ts = fi->type ? il2cpp_type_get_name(fi->type) : "";
				std::string vs = ReadValue(obj, fi, ts);
				W(std::format("    {} = {} ({} @{})", fi->name ? fi->name : "?", vs, ts, fi->offset));
				if (depth <= 0 || ts.empty()) continue;
				if (ts.rfind("System.", 0) == 0
					&& !NameHas(ts.c_str(), { "Runway", "Camerawork", "LiveCut", "Scene", "Stage" })) continue;
				if ((size_t)fi->offset < 0x10 || (size_t)fi->offset > 0x8000) continue;
				uint64_t raw = *reinterpret_cast<uint64_t*>(reinterpret_cast<uintptr_t>(obj) + (size_t)fi->offset);
				if (!PlausiblePtr(raw)) continue;
				auto child = reinterpret_cast<void*>(raw);
				auto ck = ClassOf(child);
				const char* ccn = ck ? il2cpp_class_get_name(ck) : nullptr;
				if (!ccn) continue;
				bool isArr = ts.size() >= 2 && ts[ts.size() - 1] == ']';
				if (!isArr && !NameHas(ccn, { "Runway", "Scene", "Stage", "MstEvent", "Camerawork", "LiveCut" })
					&& !NameHas(ts.c_str(), { "Runway", "Scene", "Stage", "Camerawork", "LiveCut" })) continue;
				if (isArr && !NameHas(ts.c_str(), { "Runway", "Scene", "Stage", "Mst", "Camerawork", "LiveCut" })) continue;
				ReflectDump(child, depth - 1, fi->name ? fi->name : "?", runtime);
			}
		}

		void DumpClassFields(void* klass, const char* label, void (*W)(const std::string&)) {
			if (!klass) { W(std::format("=== class {} : NOT FOUND ===", label)); return; }
			W(std::format("=== fields of {} ===", label));
			if (!API().ok) { W("  (field API unavailable)"); return; }
			void* iter = nullptr;
			void* fiRaw = nullptr;
			int cap = 0;
			while ((fiRaw = API().class_get_fields(klass, &iter)) != nullptr) {
				if (++cap > 300) break;
				auto fi = reinterpret_cast<const FieldInfo*>(fiRaw);
				uint32_t flags = API().field_get_flags(fiRaw);
				if (flags & 0x40) continue;
				std::string ts = fi->type ? il2cpp_type_get_name(fi->type) : "?";
				W(std::format("  {}{} off={} type={}", (flags & 0x10) ? "static " : "", fi->name ? fi->name : "?", fi->offset, ts));
			}
		}

		void DumpTableRows(void* table, const char* label, int maxRows = 1000) {
			ToMaster(std::format("--- table {} ---", label));
			if (!table || !PlausiblePtr((uint64_t)table)) { ToMaster("  (null)"); return; }
			const char* tcn = ClassNameOf(table);
			ToMaster(std::format("  container {} {}", tcn ? tcn : "?", PtrStr(table)));
			auto data = ReadPtr(table, "data");   // MasterTable`1.data
			const char* dcn = data && PlausiblePtr((uint64_t)data) ? ClassNameOf(data) : nullptr;
			ToMaster(std::format("  data={} {}", data ? PtrStr(data) : "(null)", dcn ? dcn : ""));
			if (!dcn) return;
			std::string c = dcn;
			if (c.find("List`1") != std::string::npos) {
				auto arr = ReadPtr(data, "_items");
				int size = ReadInt(data, "_size");
				if (size < 0) size = ArrayLength(arr);
				if (size > maxRows) size = maxRows;
				ToMaster(std::format("  list size={}", size));
				if (arr && PlausiblePtr((uint64_t)arr))
					for (int i = 0; i < size; i++) ReflectDump(*(void**)ArrayElem(arr, i, 8), 1, label, false);
			}
			else if (c.find("Dictionary`2") != std::string::npos) {
				auto entries = ReadPtr(data, "_entries");
				int count = ReadInt(data, "_count");
				if (count < 0) count = ArrayLength(entries);
				if (count > maxRows) count = maxRows;
				ToMaster(std::format("  dict count={}", count));
				if (entries && PlausiblePtr((uint64_t)entries))
					for (int i = 0; i < count; i++) {
						uintptr_t e = ArrayElem(entries, i, 24);
						if (*(int*)e < 0) continue;
						ReflectDump(*(void**)(e + 16), 1, label, false);
					}
			}
			else if (c.find("SortedList`2") != std::string::npos) {
				auto values = ReadPtr(data, "values");
				int size = ReadInt(data, "_size");
				if (size < 0) size = ArrayLength(values);
				if (size > 1000) size = 1000;
				ToMaster(std::format("  sortedlist size={}", size));
				if (values && PlausiblePtr((uint64_t)values))
					for (int i = 0; i < size; i++) ReflectDump(*(void**)ArrayElem(values, i, 8), 1, label, false);
			}
			else if (!c.empty() && c.back() == ']') {
				DumpContainerElems(data, 8, maxRows, label, 1, false);
			}
			else {
				ToMaster(std::format("  (container class {} not walkable)", c));
			}
		}

		// one-shot: dump the runway master tables + all Shiny Runway MstEvent rows
		void TryDumpMasters() {
			static bool done = false;
			static long frame = 0;
			static int tries = 0;
			if (done) return;
			if ((++frame % 90) != 1) return;
			if (++tries > 300) { done = true; return; }
			if (!API().ok) {
				if (tries == 1) ToMaster("field-enumeration API unresolved - dump skipped");
				done = true;
				return;
			}
			if (EventDates::g_runwayRows.empty()) return;

			static auto method_get_Instance = il2cpp_symbols_logged::get_method(
				"PRISM.Adapters.dll", "PRISM.Adapters", "IAdapterLocator", "get_Instance", 0);
			if (!method_get_Instance || !method_get_Instance->methodPointer) return;
			auto locator = method_get_Instance->Invoke<Il2CppObject*>(nullptr, {});
			if (!locator) return;
			auto locatorType = il2cpp_symbols::get_class_from_instance(locator);
			auto mget = locatorType ? il2cpp_class_get_method_from_name(locatorType, "get_MasterData", 0) : nullptr;
			if (!mget || !mget->methodPointer) return;
			auto masterData = mget->Invoke<Il2CppObject*>(locator, {});
			if (!masterData) return;
			done = true;

			ToMaster(std::format("=== v21 runway master dump (pid {}) ===", GetCurrentProcessId()));
			DumpClassFields(il2cpp_symbols_logged::get_class("PRISM.Definitions.dll", "PRISM.Definitions", "MstRunwayScene"), "MstRunwayScene", ToMaster);
			DumpClassFields(il2cpp_symbols_logged::get_class("PRISM.Definitions.dll", "PRISM.Definitions", "MstRunwayStage"), "MstRunwayStage", ToMaster);
			DumpClassFields(il2cpp_symbols_logged::get_class("PRISM.Definitions.dll", "PRISM.Definitions", "MstRunwayMotion"), "MstRunwayMotion", ToMaster);
			DumpClassFields(il2cpp_symbols_logged::get_class("PRISM.Definitions.dll", "PRISM.Definitions", "MstEvent"), "MstEvent", ToMaster);
			DumpClassFields(il2cpp_symbols_logged::get_class("PRISM.Definitions.dll", "PRISM.Definitions", "MstLiveCutScene"), "MstLiveCutScene", ToMaster);
			DumpClassFields(il2cpp_symbols_logged::get_class("PRISM.Definitions.dll", "PRISM.Definitions", "MstLiveCutSceneCostume"), "MstLiveCutSceneCostume", ToMaster);

			auto mk = il2cpp_symbols::get_class_from_instance(masterData);
			ToMaster("=== MasterData fields ===");
			std::vector<std::pair<std::string, void*>> tables;
			void* iter = nullptr;
			void* fiRaw = nullptr;
			int cap = 0;
			while ((fiRaw = API().class_get_fields(mk, &iter)) != nullptr && cap++ < 500) {
				auto fi = reinterpret_cast<const FieldInfo*>(fiRaw);
				const char* fn = fi->name;
				std::string ts = fi->type ? il2cpp_type_get_name(fi->type) : "?";
				void* v = il2cpp_symbols::read_field<void*>(masterData, fi);
				const char* vc = v && PlausiblePtr((uint64_t)v) ? ClassNameOf(v) : nullptr;
				ToMaster(std::format("  {} : {} = {} {}", fn ? fn : "?", ts,
					v && PlausiblePtr((uint64_t)v) ? PtrStr(v) : "(null)", vc ? vc : ""));
				if (fn && (strstr(fn, "Runway") || strstr(fn, "LiveCutScene")) && v && PlausiblePtr((uint64_t)v))
					tables.emplace_back(fn, v);
			}
			for (auto& kv : tables)
				DumpTableRows(kv.second, kv.first.c_str(),
					NameHas(kv.first.c_str(), { "LiveCutScene" }) ? 400 : 1000);

			ToMaster("=== MstEvent runway rows (all fields) ===");
			for (auto& r : EventDates::g_runwayRows) {
				ToMaster(std::format("-- event row id={} @ {}", r.first, PtrStr(r.second)));
				ReflectDump(r.second, 1, "MstEvent", false);
			}
			ToMaster("=== v21 master dump complete ===");
		}

		// v22: last runway event id seen by RunwayUnitConfirmationModel ctor
		int g_lastEventId = 0;   // [DIAG] v22

		// v22: runtime scene override test switch (env SCSP_RUNWAY_SCENE=900_DressO_Summer etc.)
		void ForceSceneName(Il2CppObject* data) {
			static std::string forced = [] {
				const char* v = getenv("SCSP_RUNWAY_SCENE");
				return v ? std::string(v) : std::string();
			}();
			if (forced.empty() || !data) return;
			try {
				auto k = ClassOf(data);
				const char* cn = k ? il2cpp_class_get_name(k) : nullptr;
				if (!cn || !strstr(cn, "RunwayEventStartData")) return;
				auto fi = il2cpp_symbols_logged::il2cpp_class_get_field_from_name(k, "<SceneName>k__BackingField");
				if (!fi) { ToRuntime("[scene-force] <SceneName> field not found"); return; }
				auto s = il2cpp_string_new(forced.c_str());
				il2cpp_symbols::write_field<Il2CppString*>(data, fi, s);
				ToRuntime(std::format("[scene-force] SceneName -> \"{}\"", forced));
			}
			catch (...) {}
		}

		// runtime: every RunwayEventStartData handed to PreLoadAsync
		void DumpStartData(Il2CppObject* data) {
			if (!data) return;
			try {
				auto k = ClassOf(data);
				const char* cn = k ? il2cpp_class_get_name(k) : nullptr;
				if (!cn) return;
				const bool runway = NameHas(cn, { "Runway" });
				const bool startData = NameHas(cn, { "StartData" });
				if (!runway && !startData) return;
				if (!runway && g_startDataClasses >= 2) return;
				static std::unordered_set<uintptr_t> seen;
				if (!seen.insert((uintptr_t)data).second) return;
				if (seen.size() > 200) return;
				if (!runway) g_startDataClasses++;
				ToRuntime(std::format("=== start data #{} {} event={} @ {} ===",
					(int)seen.size(), cn, g_lastEventId, PtrStr(data)));
				ReflectDump(data, 4, cn, true);
			}
			catch (...) {}
		}
	}

	// v13: the four MstEvent getter hooks were removed - those addresses are
	// ICF-shared with unrelated methods and hooking them crashed the game
	// (GameAssembly.dll+0x20af266).  All date inspection is pure field reads.

	HOOK_ORIG_TYPE DioramaProductViewModel_IsLocked_orig;
	bool DioramaProductViewModel_IsLocked_hook(void* self) {
		if (g_unlock_photo_studio) {
			return false;
		}
		return HOOK_CAST_CALL(bool, DioramaProductViewModel_IsLocked)(self);
	}

	HOOK_ORIG_TYPE RunwayPoseSettingContentViewModel_ctor_orig;
	void RunwayPoseSettingContentViewModel_ctor_hook(void* self, void* motion, int position) {
		HOOK_CAST_CALL(void, RunwayPoseSettingContentViewModel_ctor)(self, motion, position);

		static auto klass_MstRunwayMotion = il2cpp_symbols_logged::get_class("PRISM.Definitions.dll", "PRISM.Definitions", "MstRunwayMotion");
		static auto method_MstRunwayMotion_get_Id = il2cpp_symbols_logged::get_method(klass_MstRunwayMotion, "get_Id", 0);
		static auto method_MstRunwayMotion_get_MotionName = il2cpp_symbols_logged::get_method(klass_MstRunwayMotion, "get_MotionName", 0);
		static auto method_MstRunwayMotion_get_IsPose = il2cpp_symbols_logged::get_method(klass_MstRunwayMotion, "get_IsPose", 0);
		static auto method_MstRunwayMotion_get_IsWalkingPattern = il2cpp_symbols_logged::get_method(klass_MstRunwayMotion, "get_IsWalkingPattern", 0);

		const auto managedMotion = (Il2CppObject*)motion;

		static bool motionSaved = false;
		static bool headerPrinted = false;

		if (!motionSaved) {
			if (!headerPrinted) {
				std::cout << "=== Runway Motions ===" << std::endl << "Id\tName\tIsWalking\tIsPose\n";
				headerPrinted = true;
			}

			auto id = method_MstRunwayMotion_get_Id->Invoke(managedMotion, {})->unbox_value<int>();
			auto name = method_MstRunwayMotion_get_MotionName->Invoke<Il2CppString*>(managedMotion, {})->ToUtf8String();
			auto isWalking = method_MstRunwayMotion_get_IsWalkingPattern->Invoke(managedMotion, {})->unbox_value<bool>();
			auto isPose = method_MstRunwayMotion_get_IsPose->Invoke(managedMotion, {})->unbox_value<bool>();

			if (knownRunwayMotions.find(id) != knownRunwayMotions.end()) {
				motionSaved = true;
			}
			else {
				std::cout
					<< id << '\t' << name << '\t'
					<< (isWalking ? "O" : "X") << '\t' << (isPose ? "O" : "X")
					<< std::endl;

				RunwayMotion savedata{
					.Id = id,
					.Name = name,
					.IsWalking = isWalking,
					.IsPose = isPose
				};
				knownRunwayMotions.emplace(id, savedata);
			}
		}
	}

	HOOK_ORIG_TYPE LocalizationManager_GetTextOrNull_orig;
	Il2CppString* LocalizationManager_GetTextOrNull_hook(void* _this, Il2CppString* category, int id) {
		if (g_max_fps != -1) set_fps_hook(g_max_fps);
		if (g_vsync_count != -1) set_vsync_count_hook(g_vsync_count);
		itLocalizationManagerDic(_this);
		std::string resultText = "";
		if (SCLocal::getLocalifyText(utility::conversions::to_utf8string(category->start_char), id, &resultText)) {
			//updateDicText(_this, category, id, resultText);
			return il2cpp_string_new(resultText.c_str());
		}
		// GG category: mlPublicText_GameGuard, id: 1
		return HOOK_CAST_CALL(Il2CppString*, LocalizationManager_GetTextOrNull)(_this, category, id);
	}

	HOOK_ORIG_TYPE GetResolutionSize_orig;
	Vector2Int_t GetResolutionSize_hook(void* camera, void* method) {
		auto ret = HOOK_CAST_CALL(Vector2Int_t, GetResolutionSize)(camera, method);
		if (g_3d_resolution_scale != 1.0f) {
			ret.x *= g_3d_resolution_scale;
			ret.y *= g_3d_resolution_scale;
			SCCamera::currRenderResolution.x = ret.x;
			SCCamera::currRenderResolution.y = ret.y;
		}
		return ret;
	}

	static const char* UnityShaderUsualPropertyNames[] = {
		"_MainTex",
		"_BaseMap",
		"_Color",
		"_BaseColor",
		"_TintColor",
		"_MainTex_ST",
		"_NormalMap",
		"_BumpMap",
		"_Metallic",
		"_Glossiness",
		"_Smoothness",
		"_SpecColor",
		"_SpecularColor",
		"_EmissionColor",
		"_EmissionMap",
		"_OcclusionMap",
		"_HeightMap",
		"_ParallaxMap",
		"_Cutoff",
		"_AlphaClipThreshold",
		"_DetailAlbedoMap",
		"_DetailNormalMap",
		"_RimColor",
		"_RimPower",
		"_EmissionScaleUI",
		"_GlossMapScale",
		"_Time",
		"_SinTime",
		"_CosTime"
	};
	static std::vector<int> UnityShaderUsualPropertyNameIds{};

	std::unordered_set<std::string> dumpedTextureNames{};
	std::map<std::string, std::vector<int>> shaderPropIds{};

	Il2CppObject* CreateNewTexure2D(int width, int height) {
		static auto klass_Texture2D = il2cpp_symbols::get_class("UnityEngine.CoreModule.dll", "UnityEngine", "Texture2D");
		static auto ctor_Texture2D_2 = il2cpp_class_get_method_from_name(klass_Texture2D, ".ctor", 2);
		auto tex = (Il2CppObject*)il2cpp_object_new(klass_Texture2D);
		void* args_ctor_Texture2D_2[2]{ &width, &height };
		reflection::Invoke(ctor_Texture2D_2, tex, (Il2CppObject**)args_ctor_Texture2D_2, "DumpTexture2D|new Texture2D");
		return tex;
	}

	void PrintProgressBar(uint32_t i, uint32_t upper, bool withLeadingReturn) {
		if (withLeadingReturn) printf("\r");
		float percent = i / (float)upper * 100.f;
		int charCount = (int)(percent / 10);
		printf("[");
		for (int c = 0; c < charCount; ++c) { printf("="); }
		for (int c = charCount; c < 10; ++c) { printf(" "); }
		printf("] %.4f%% (%u)", percent, i);
	}

	void SaveShaderTextureIds(const std::string& shaderName, const std::vector<int>& propIds) {
		if (!std::filesystem::exists(g_localify_base)) {
			std::cerr << "[ERROR] Failed to save shader data. (root data directory \"" << g_localify_base << "\" doesn't exist)" << std::endl;
			return;
		}

		auto filepath = g_localify_base / "shaderdata";
		if (!std::filesystem::exists(filepath)) {
			if (!std::filesystem::create_directory(filepath)) {
				std::cerr << "[ERROR] Failed to save shader data. (failed to create data directory)" << std::endl;
				return;
			}
		}
		filepath /= shaderName;

		rapidjson::Document doc;
		doc.SetArray();
		auto& allocator = doc.GetAllocator();
		for (int id : propIds) {
			doc.PushBack(id, allocator);
		}

		rapidjson::StringBuffer buffer;
		rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
		doc.Accept(writer);

		std::ofstream ofs(filepath);
		if (!ofs.is_open()) {
			std::cerr << "[ERROR] Failed to save shader data. (failed to open the file " << filepath << std::endl;
			return;
		}
		ofs << buffer.GetString();
		if (!ofs) {
			std::cerr << "[ERROR] Failed to save shader data. (failed to write contents)" << "\n";
			return;
		}
	}

	void DetectShaderTextureIds(const std::string& shaderName, Il2CppObject* material, std::vector<int>& propIds, uint32_t upper = 0) {
		static auto method_Material_PropertyToID = il2cpp_symbols_logged::get_method("UnityEngine.CoreModule.dll", "UnityEngine", "Shader", "PropertyToID", 1);
		if (UnityShaderUsualPropertyNameIds.size() == 0) {
			auto length = sizeof(UnityShaderUsualPropertyNames) / sizeof(UnityShaderUsualPropertyNames[0]);
			for (int i = 0; i < length; ++i) {
				auto name = (Il2CppObject*)il2cpp_string_new(UnityShaderUsualPropertyNames[i]);
				auto boxed = reflection::Invoke(method_Material_PropertyToID, nullptr, &name, "DetectShaderTextureIds|Material::PropertyToID");
				auto id = il2cpp_symbols::unbox<int>(boxed);
				UnityShaderUsualPropertyNameIds.push_back(id);
			}
		}

		static auto method_Material_GetTextureImpl = il2cpp_symbols_logged::get_method("UnityEngine.CoreModule.dll", "UnityEngine", "Material", "GetTextureImpl", 1);
		if (upper == 0) upper = UINT32_MAX;
		auto startTime = std::chrono::steady_clock::now();
		auto lastTime = startTime;
		PrintProgressBar(0, upper, false);
		int i = 0;
	LOOP_START:
		for (; i <= upper; ++i) {
			auto now = std::chrono::steady_clock::now();
			if (now - lastTime >= std::chrono::seconds(1)) {
				PrintProgressBar(i, upper, true);
				lastTime = now;
			}

			auto pi = &i;
			auto t = reflection::Invoke(method_Material_GetTextureImpl, material, (Il2CppObject**)&pi, "DetectShaderTextureIds|Material::GetTextureImpl");
			if (t != nullptr) {
				propIds.push_back(i);
				printf("\rTextureId found at %i          \n", i);
			}
		}
		if (upper < UINT32_MAX) {
			for (int knownId : UnityShaderUsualPropertyNameIds) {
				if (knownId > upper) {
					auto pi = &knownId;
					auto t = reflection::Invoke(method_Material_GetTextureImpl, material, (Il2CppObject**)&pi, "DetectShaderTextureIds|Material::GetTextureImpl");
					if (t != nullptr) {
						propIds.push_back(i);
						printf("\rTextureId found at %i          \n", i);
					}
				}
			}
		}
		PrintProgressBar(upper, upper, true);
		printf("\n");
		if (upper == UINT32_MAX) {
			SaveShaderTextureIds(shaderName, propIds);
		}
		else {
			printf("[INFO] Shader data won't be saved for a quick probing.\n");
		}
	}
	bool DetectShaderTextureIdsUnsafe(const std::string& shaderName, Il2CppObject* material, std::vector<int>& propIds) {
		bool isFirst = true;
		for (auto id : propIds) {
			if (!isFirst) std::cout << ", ";
			std::cout << id;
			isFirst = false;
		}
		std::cout << "}" << std::endl;

		auto success = material_texture_ids_sim::enumerate_texture_property_name_ids_from_managed_material(
			material, propIds
		);

		isFirst = true;
		for (auto id : propIds) {
			if (!isFirst) std::cout << ", ";
			std::cout << id;
			isFirst = false;
		}
		std::cout << "}" << std::endl;
		return success;
	}

	bool TryLoadShaderData(const std::string& shaderName) {
		auto filepath = g_localify_base / "shaders" / shaderName;

		if (!std::filesystem::exists(filepath)) {
			return false;
		}

		std::ifstream ifs(filepath, std::ios::binary);
		if (!ifs.is_open()) {
			std::cerr << "[ERROR] Failed to open file " << filepath << std::endl;
			return false;
		}

		std::string content;
		content.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

		rapidjson::Document doc;
		doc.Parse(content.c_str());
		if (doc.HasParseError()) {
			std::cerr << "[ERROR] Failed to deserialize " << filepath << std::endl;
			return false;
		}

		if (!doc.IsArray()) {
			std::cerr << "[ERROR] Invalid shader data " << filepath << std::endl;
			return false;
		}

		std::vector<int> propIds{};
		for (rapidjson::SizeType i = 0; i < doc.Size(); ++i) {
			const rapidjson::Value& v = doc[i];
			if (!v.IsInt()) {
				std::cerr << "[ERROR] Invalid shader data " << filepath << " (element " << i << ")" << std::endl;
				continue;
			}
			int value = v.GetInt();
			propIds.push_back(value);
		}

		shaderPropIds.insert_or_assign(shaderName, std::move(propIds));
		return true;
	}

	void LoadReplacementTextures() {
		const auto filepath = g_localify_base / "textures";
		if (!std::filesystem::exists(filepath))
			return;
		for (const auto& entry : std::filesystem::directory_iterator(filepath)) {
			if (entry.is_regular_file()) {
				std::filesystem::path filePath = entry.path();
				std::string stem = filePath.stem().string();
				std::string name = filePath.filename().string();
				replacementTexureNames[stem] = name;
			}
		}
	}

	void ReplaceMaterialTexture(Il2CppObject* meterial, int width, int height, std::string texName, int texId) {
		static auto method_File_ReadAllBytes = il2cpp_symbols_logged::get_method_corlib(
			"System.IO", "File", "ReadAllBytes", 1
		);
		static auto method_ImageConversion_LoadImage_2 = il2cpp_symbols_logged::get_method(
			"UnityEngine.ImageConversionModule.dll", "UnityEngine", "ImageConversion", "LoadImage", 2
		);
		static auto method_Material_SetTextureImpl = il2cpp_symbols_logged::get_method(
			"UnityEngine.CoreModule.dll", "UnityEngine", "Material", "SetTextureImpl", 2
		);

		auto it = replacementTexureNames.find(texName);
		if (it != replacementTexureNames.end()) {
			auto replacementFilepath = g_localify_base / "textures" / it->second;
			if (std::filesystem::exists(replacementFilepath)) {
				auto tex = CreateNewTexure2D(width, height);
				auto managedFilepath = (Il2CppObject*)il2cpp_string_new(replacementFilepath.string().c_str());
				auto bytes = reflection::Invoke(method_File_ReadAllBytes, nullptr, &managedFilepath, "LoadReplacementTexture|File::ReadAllBytes");
				Il2CppObject* args_ImageConversion_LoadImage_2[2]{ tex, bytes };
				reflection::InvokeVoid(method_ImageConversion_LoadImage_2, nullptr, args_ImageConversion_LoadImage_2, "LoadReplacementTexture|ImageConversion::LoadImage");
				Il2CppObject* args_Material_SetTextureImpl[2]{ (Il2CppObject*)(&texId), tex };
				reflection::InvokeVoid(method_Material_SetTextureImpl, meterial, args_Material_SetTextureImpl, "LoadReplacementTexture|Material::SetTextureImpl");
				printf("Texture '%s' is replaced.\n", texName.c_str());
			}
		}
	}

	// reference: https://github.com/Kimjio/imas-sc-prism-localify
	// returns: name of the texture
	std::string DumpTexture2D(Il2CppObject* texture, std::string type, int* pOutWidth = nullptr, int* pOutHeight = nullptr) {
		if (texture == nullptr) return "";

		static int32_t zero = 0, one = 1;
		static auto klass_Int32 = il2cpp_symbols_logged::get_class_corlib("System", "Int32");
		static auto klass_Rect = (Il2CppClass*)il2cpp_symbols_logged::get_class("UnityEngine.CoreModule.dll", "UnityEngine", "Rect");
		static auto klass_RenderTextureFormat = (Il2CppClass*)il2cpp_symbols_logged::get_class("UnityEngine.CoreModule.dll", "UnityEngine", "RenderTextureFormat");
		static auto klass_RenderTextureReadWrite = (Il2CppClass*)il2cpp_symbols_logged::get_class("UnityEngine.CoreModule.dll", "UnityEngine", "RenderTextureReadWrite");

		static auto method_UnityObject_getname = il2cpp_symbols_logged::get_method(
			"UnityEngine.CoreModule.dll", "UnityEngine",
			"Object", "get_name", 0);
		auto textureName = reflection::Invoke<Il2CppString*>(method_UnityObject_getname, texture, nullptr, "DumpTexture2D|Object::get_name");

		std::string strTextureName = reflection::helper::ToUtf8(textureName);
		if (dumpedTextureNames.contains(strTextureName)) {
			return strTextureName;
		}

		auto method_getwidth = il2cpp_class_get_method_from_name(texture->klass, "get_width", 0);
		auto method_getheight = il2cpp_class_get_method_from_name(texture->klass, "get_height", 0);
		auto managedWidth = reflection::Invoke(method_getwidth, texture, nullptr, "DumpTexture2D|Texture2D::get_width");
		auto managedHeight = reflection::Invoke(method_getheight, texture, nullptr, "DumpTexture2D|Texture2D::get_height");
		auto width = il2cpp_symbols::unbox<int>(managedWidth);
		auto height = il2cpp_symbols::unbox<int>(managedHeight);

		if (pOutWidth != nullptr) {
			*pOutWidth = width;
		}
		if (pOutHeight != nullptr) {
			*pOutHeight = height;
		}

		// var renderTexture = RenderTexture.GetTemporary(width, height, 0, 0, 1);
		static auto method_RenderTexture_GetTemporary_5 = il2cpp_symbols_logged::get_method(
			"UnityEngine.CoreModule.dll", "UnityEngine",
			"RenderTexture", "GetTemporary", 5);
		void* args_RenderTexture_GetTemporary_5[5]{ &width, &height, &zero, &zero, &one };
		auto renderTexture = reflection::Invoke(method_RenderTexture_GetTemporary_5, nullptr, (Il2CppObject**)args_RenderTexture_GetTemporary_5, "DumpTexture2D|RenderTexture::GetTemporary");

		// Graphics.Blit(texture, renderTexture);
		static auto method_Graphics_Blit_2 = il2cpp_symbols_logged::get_method("UnityEngine.CoreModule.dll", "UnityEngine", "Graphics", "Blit", 2);
		Il2CppObject* args_Graphics_Blit_2[2]{ texture, renderTexture };
		reflection::InvokeVoid(method_Graphics_Blit_2, nullptr, args_Graphics_Blit_2, "DumpTexture2D|Graphics::Blit2");

		// var previous = RenderTexture.active;
		static auto method_RenderTexture_getactive = il2cpp_symbols_logged::get_method("UnityEngine.CoreModule.dll", "UnityEngine", "RenderTexture", "get_active", 0);
		auto previous = reflection::Invoke(method_RenderTexture_getactive, nullptr, nullptr, "DumpTexture2D|RenderTexture::get_active");

		// RenderTexture.active = renderTexture;
		static auto method_RenderTexture_setactive = il2cpp_symbols_logged::get_method("UnityEngine.CoreModule.dll", "UnityEngine", "RenderTexture", "set_active", 1);
		reflection::InvokeVoid(method_RenderTexture_setactive, nullptr, &renderTexture, "DumpTexture2D|RenderTexture::set_active");

		// var readableTexture = new Texture2D(width, height);
		auto readableTexture = CreateNewTexure2D(width, height);

		// readableTexture.ReadPixels(new Rect(0, 0, width, height), 0, 0);
		static auto method_Texture2D_ReadPixels = il2cpp_symbols_logged::get_method("UnityEngine.CoreModule.dll", "UnityEngine", "Texture2D", "ReadPixels", 3);
		auto rect = Rect_t{ 0.f, 0.f, (float)width, (float)height };
		void* args_Texture2D_ReadPixels[3]{ &rect , &zero, &zero };
		reflection::InvokeVoid(method_Texture2D_ReadPixels, readableTexture, (Il2CppObject**)args_Texture2D_ReadPixels, "DumpTexture2D|Texture2D::ReadPixels");

		// readableTexture.Apply();
		static auto method_Texture2D_Apply = il2cpp_symbols_logged::get_method("UnityEngine.CoreModule.dll", "UnityEngine", "Texture2D", "Apply", 0);
		reflection::InvokeVoid(method_Texture2D_Apply, readableTexture, nullptr, "DumpTexture2D|Texture2D::Apply");

		// RenderTexture.set_active(previous);
		reflection::InvokeVoid(method_RenderTexture_setactive, nullptr, &previous, "DumpTexture2D|RenderTexture::set_active(previous)");
		// RenderTexture.ReleaseTemporary(renderTexture);
		static auto method_RenderTexture_ReleaseTemporary = il2cpp_symbols_logged::get_method("UnityEngine.CoreModule.dll", "UnityEngine", "RenderTexture", "ReleaseTemporary", 1);
		reflection::InvokeVoid(method_RenderTexture_ReleaseTemporary, nullptr, &renderTexture, "DumpTexture2D|RenderTexture::ReleaseTemporary");

		// var pngData = ImageConversion.EncodeToPNG(readableTexture);
		static auto method_ImageConversion_EncodeToPNG = il2cpp_symbols::get_method("UnityEngine.ImageConversionModule.dll", "UnityEngine", "ImageConversion", "EncodeToPNG", 1);
		auto pngData = reflection::Invoke<Il2CppArray*>(method_ImageConversion_EncodeToPNG, nullptr, &readableTexture, "DumpTexture2D|ImageConversion::EncodeToPNG");

		static auto klass_String = il2cpp_symbols_logged::get_class_corlib("System", "String");
		static auto method_String_Replace = il2cpp_symbols::find_method(
			klass_String,
			[](const MethodInfo* mi) {
				if (2 != il2cpp_method_get_param_count(mi)) return false;
				return 0 == strcmp("String", il2cpp_symbols::il2cpp_method_get_param_type_name(mi, 0));
			});
		Il2CppObject* args_String_Replace[2]{ (Il2CppObject*)il2cpp_string_new("|"), (Il2CppObject*)il2cpp_string_new("_") };
		reflection::Invoke(method_String_Replace, (Il2CppObject*)textureName, args_String_Replace, "DumpTexture2D|String::Replace");

		static auto method_Directory_Exists = il2cpp_symbols_logged::get_method_corlib("System.IO", "Directory", "Exists", 1);
		static auto method_Directory_CreateDirectory = il2cpp_symbols_logged::get_method_corlib("System.IO", "Directory", "CreateDirectory", 1);
		static auto method_File_WriteAllBytes = il2cpp_symbols_logged::get_method_corlib("System.IO", "File", "WriteAllBytes", 2);

		Il2CppObject* mstr;

		mstr = (Il2CppObject*)il2cpp_string_new("TextureDump");
		if (!il2cpp_symbols::unbox<bool>(
			reflection::Invoke(method_Directory_Exists, nullptr, &mstr, "DumpTexture2D|Directory::Exists")
		)) {
			reflection::Invoke(method_Directory_CreateDirectory, nullptr, &mstr, "DumpTexture2D|Directory::CreateDirectory");
		}

		mstr = (Il2CppObject*)il2cpp_string_new(("TextureDump/" + type).c_str());
		if (!il2cpp_symbols::unbox<bool>(
			reflection::Invoke(method_Directory_Exists, nullptr, &mstr, "DumpTexture2D|Directory::Exists")
		)) {
			reflection::Invoke(method_Directory_CreateDirectory, nullptr, &mstr, "DumpTexture2D|Directory::CreateDirectory");
		}

		// File.WriteAllBytes(saveFilePath, pngData);
		std::string saveFilePath = "TextureDump/" + type + "/" + strTextureName;
		if (strTextureName.find(".png") == std::string::npos) {
			saveFilePath += ".png";
		}

		Il2CppObject* args_File_WriteAllBytes[2]{
			(Il2CppObject*)il2cpp_string_new(saveFilePath.c_str()),
			pngData
		};
		std::cout << "[DumpTexture2D] " << saveFilePath << std::endl;
		reflection::Invoke(method_File_WriteAllBytes, nullptr, args_File_WriteAllBytes, "DumpTexture2D|File::WriteAllBytes");
		dumpedTextureNames.emplace(strTextureName);

		return strTextureName;
	}

	// =====================================================================
	// [DIAG] v23 - runway scene discovery: while a runway run is playing,
	// scan loaded TMP texts + textures, log names/texts to
	// runway-runtime-dump.txt and export candidate textures as PNG into
	// TextureDump/scene-scan/ so the wall/screen art source can be identified.
	// =====================================================================
	namespace RunwaySceneScan {
		int g_countdown = -1;
		int g_pass = 0;
		int g_dumped = 0;
		std::unordered_set<uintptr_t> g_seenTmp;
		std::unordered_set<uintptr_t> g_seenTex;
		std::unordered_set<std::string> g_seenNames;
		std::vector<void*> g_dumpQueue;

		void Log(const std::string& l) { RunwayDump::ToRuntime(l); }

		bool NameHasAny(const std::string& s, std::initializer_list<const char*> keys) {
			for (auto k : keys) if (s.find(k) != std::string::npos) return true;
			return false;
		}

		Il2CppArray* FindAllOfType(void* k) {
			static auto m = il2cpp_symbols_logged::get_method("UnityEngine.CoreModule.dll", "UnityEngine", "Resources", "FindObjectsOfTypeAll", 1);
			if (!m || !m->methodPointer || !k) return nullptr;
			auto type = il2cpp_type_get_object(il2cpp_class_get_type(k));
			if (!type) return nullptr;
			return m->Invoke<Il2CppArray*>(nullptr, { (Il2CppObject*)type });
		}

		std::string ObjName(void* obj) {
			static auto getName = il2cpp_symbols_logged::get_method("UnityEngine.CoreModule.dll", "UnityEngine", "Object", "get_name", 0);
			if (!getName || !getName->methodPointer) return "";
			auto s = getName->Invoke<Il2CppString*>((Il2CppObject*)obj, {});
			return s ? s->ToUtf8String() : std::string();
		}

		void ScanTmpClass(const char* cls) {
			auto k = il2cpp_symbols_logged::get_class("Unity.TextMeshPro.dll", "TMPro", cls);
			if (!k) { Log(std::format("  [tmp-scan] {} class NOT FOUND", cls)); return; }
			auto getText = il2cpp_class_get_method_from_name(k, "get_text", 0);
			auto arr = FindAllOfType(k);
			int n = arr ? RunwayDump::ArrayLength(arr) : 0;
			int cap = n > 400 ? 400 : n;
			int matched = 0;
			for (int i = 0; i < cap; i++) {
				void* obj = *reinterpret_cast<void**>(RunwayDump::ArrayElem(arr, i, 8));
				if (!obj) continue;
				if (!g_seenTmp.insert((uintptr_t)obj).second) continue;
				std::string txt;
				if (getText && getText->methodPointer) {
					auto ts = getText->Invoke<Il2CppString*>((Il2CppObject*)obj, {});
					if (ts) txt = ts->ToUtf8String();
				}
				if (NameHasAny(txt, { "ShowPiece", "Shiny", "SHINY", "Runway", "RUNWAY", "Collection", "2026", "Spring", "Summer", "DressO" })) {
					matched++;
					Log(std::format("  [tmp] {} obj=\"{}\" text=\"{}\"", cls, ObjName(obj), txt));
				}
			}
			Log(std::format("  [tmp-scan] {} loaded={} scannedNew={} matched={}", cls, n, cap, matched));
		}

		void ScanTextures(const char* cls) {
			auto k = il2cpp_symbols_logged::get_class("UnityEngine.CoreModule.dll", "UnityEngine", cls);
			if (!k) { Log(std::format("  [tex-scan] {} NOT FOUND", cls)); return; }
			auto arr = FindAllOfType(k);
			int n = arr ? RunwayDump::ArrayLength(arr) : 0;
			int cap = n > 800 ? 800 : n;
			int matched = 0;
			for (int i = 0; i < cap; i++) {
				void* obj = *reinterpret_cast<void**>(RunwayDump::ArrayElem(arr, i, 8));
				if (!obj) continue;
				if (!g_seenTex.insert((uintptr_t)obj).second) continue;
				std::string nm = ObjName(obj);
				if (nm.empty()) continue;
				if (g_seenNames.insert(nm).second)
					Log(std::format("  [tex-name] {} {}", cls, nm));
				if (NameHasAny(nm, { "DressO", "Logo", "Title", "ShowPiece", "Wall", "Screen", "Board", "Banner", "Panel", "Sign", "Monitor", "2026", "Runway", "Label" })) {
					matched++;
					if (g_dumped + (int)g_dumpQueue.size() < 24) g_dumpQueue.push_back(obj);
				}
			}
			Log(std::format("  [tex-scan] {} loaded={} scannedNew={} matched={}", cls, n, cap, matched));
		}

		void TickSceneScan() {
			if (!g_dumpQueue.empty()) {
				void* ptr = g_dumpQueue.front();
				g_dumpQueue.erase(g_dumpQueue.begin());
				try {
					g_dumped++;
					DumpTexture2D((Il2CppObject*)ptr, "scene-scan");
				}
				catch (...) {}
				return;
			}
			if (g_countdown < 0) return;
			if (g_countdown > 0) { g_countdown--; return; }
			g_pass++;
			Log(std::format("=== runway scene scan pass {} (event={}) ===", g_pass, RunwayDump::g_lastEventId));
			try {
				ScanTmpClass("TextMeshProUGUI");
				ScanTmpClass("TextMeshPro");
				ScanTextures("Texture2D");
				ScanTextures("RenderTexture");
			}
			catch (...) { Log("  [scene-scan] error during scan"); }
			if (g_pass == 1) g_countdown = 1800;
			else { g_countdown = -1; Log("=== runway scene scan done ==="); }
		}

		void ArmSceneScan() {
			g_countdown = 900;
			g_pass = 0;
			Log(std::format("[scene-scan] armed (event={})", RunwayDump::g_lastEventId));
		}
	}

	void DumpSprite(Il2CppObject* sprite, std::string sourceType) {
		if (sprite == nullptr) return;
		auto method_gettexture = il2cpp_class_get_method_from_name(sprite->klass, "get_texture", 0);
		auto texture = reflection::Invoke(method_gettexture, sprite, nullptr, "DumpSprite|Sprite::get_texture");
		DumpTexture2D(texture, sourceType);
	}

	void DumpMaterial(Il2CppObject* material, std::string sourceType) {
		if (material == nullptr) return;

		static auto method_Material_GetTextureImpl = il2cpp_symbols_logged::get_method("UnityEngine.CoreModule.dll", "UnityEngine", "Material", "GetTextureImpl", 1);
		static auto method_Material_getshader = il2cpp_symbols_logged::get_method("UnityEngine.CoreModule.dll", "UnityEngine", "Material", "get_shader", 0);
		auto shader = reflection::Invoke(method_Material_getshader, material, nullptr, "DumpMaterial|Material::get_shader");
		static auto method_Material_getname = il2cpp_symbols_logged::get_method("UnityEngine.CoreModule.dll", "UnityEngine", "Material", "get_name", 0);
		auto managedShaderName = reflection::Invoke<Il2CppString*>(method_Material_getname, shader, nullptr, "DumpMaterial|Object::get_name");
		auto shaderName = reflection::helper::ToUtf8(managedShaderName);
		std::replace(shaderName.begin(), shaderName.end(), '/', '$');

		auto it = shaderPropIds.find(shaderName);
		if (it == shaderPropIds.end()) {
			if (TryLoadShaderData(shaderName)) {
				printf("Shader data '%s' is loaded from local file.\n", shaderName.c_str());
				it = shaderPropIds.find(shaderName);
			}
			if (it == shaderPropIds.end()) {
				std::vector<int> propIds{};
				uint32_t upper = g_shader_quickprobing ? 8192 : 0;
				printf("Detecting shader '%s'... (%i)\n", shaderName.c_str(), upper);
				if (!(g_shader_unsafe_probing && DetectShaderTextureIdsUnsafe(shaderName, material, propIds))) {
					if (g_shader_unsafe_probing) {
						printf("Failed to probe unsafely. Fall back to normal probing...\n");
						propIds.clear();
					}
					DetectShaderTextureIds(shaderName, material, propIds, upper);
				}
				shaderPropIds.emplace(shaderName, propIds);
				it = shaderPropIds.find(shaderName);
			}
		}
		if (it != shaderPropIds.end()) {
			for (int id : it->second) {
				auto pi = &id;
				auto texture = reflection::Invoke(method_Material_GetTextureImpl, material, (Il2CppObject**)&pi, "DumpMaterial|Material::GetTextureImpl");
				int width = 0, height = 0;
				auto texName = DumpTexture2D(texture, sourceType, &width, &height);
				ReplaceMaterialTexture(material, width, height, texName, id);
			}
		}
	}

	void ExtractAsset(Il2CppObject* obj, Il2CppString* name) {
		static auto klass_Renderer = il2cpp_symbols_logged::get_class("UnityEngine.CoreModule.dll", "UnityEngine", "Renderer");

		if (0 == strcmp(obj->klass->name, "Image")) {
			if (g_extract_asset_image) {
				auto method_getsprite = il2cpp_class_get_method_from_name(obj->klass, "get_sprite", 0);
				auto sprite = reflection::Invoke(method_getsprite, obj, nullptr, "ExtractAsset|Image::get_sprite");
				DumpSprite(sprite, "Image");
			}
		}
		else if (0 == strcmp(obj->klass->name, "RawImage")) {
			if (g_extract_asset_rawimage) {
				auto method_gettexture = il2cpp_class_get_method_from_name(obj->klass, "get_texture", 0);
				auto texture = reflection::Invoke(method_gettexture, obj, nullptr, "ExtractAsset|RawImage::get_texture");
				DumpTexture2D(texture, "RawImage");
			}
		}
		else if (il2cpp_class_is_assignable_from(klass_Renderer, obj->klass)) {
			if (g_extract_asset_renderer) {
				static auto method_Renderer_getsharedMaterial = il2cpp_symbols_logged::get_method("UnityEngine.CoreModule.dll", "UnityEngine", "Renderer", "get_sharedMaterial", 0);
				static auto method_Renderer_getsharedMaterials = il2cpp_symbols_logged::get_method("UnityEngine.CoreModule.dll", "UnityEngine", "Renderer", "get_sharedMaterials", 0);

				auto sharedMaterials = reflection::Invoke(method_Renderer_getsharedMaterials, obj, nullptr, "ExtractAsset|Renderer::get_sharedMaterials");
				auto length = il2cpp_array_length(sharedMaterials);
				for (uint32_t i = 0; i < length; ++i) {
					auto material = (Il2CppObject*)il2cpp_symbols::array_get_value(sharedMaterials, i);
					DumpMaterial(material, obj->klass->name);
				}
			}
		}
		else if (0 == strcmp(obj->klass->name, "Sprite")) {
			if (g_extract_asset_sprite) {
				DumpSprite(obj, "Sprite");
			}
		}
		else if (0 == strcmp(obj->klass->name, "Texture")) {
			if (g_extract_asset_texture2d) {
				DumpTexture2D(obj, "Texture2D");
			}
		}
		else if (g_extract_asset_log_unknown_asset) {
			printf("  [Asset] %s : %s\n", reflection::helper::ToUtf8(name).c_str(), obj->klass->name);
		}
	}

	void ExtractAssetsGameObject(Il2CppObject* obj, Il2CppString* name) {
		if (!obj) return;

		if (0 == strcmp(obj->klass->name, "GameObject")) {
			static auto method_GameObject_GetComponentsInternal = il2cpp_symbols::get_method(
				"UnityEngine.CoreModule.dll", "UnityEngine",
				"GameObject", "GetComponentsInternal", 6
			);
			static auto type_UnityObject = reflection::typeof("UnityEngine.CoreModule.dll", "UnityEngine", "Object");
			static auto func_GameObject_GetComponentsInternal = reinterpret_cast<
				Il2CppArray * (*)(
					Il2CppObject * instance,
					void* cppTypeObject,
					bool useSearchTypeAsArrayReturnType,
					bool recursive,
					bool includeInactive,
					bool reverse,
					void* resultList)
			>(method_GameObject_GetComponentsInternal->methodPointer);
			auto components = func_GameObject_GetComponentsInternal(obj, type_UnityObject, true, true, true, false, nullptr);
			auto length = il2cpp_array_length(components);
			for (uint32_t i = 0; i < length; ++i) {
				const auto item = (Il2CppObject*)il2cpp_symbols::array_get_value(components, i);
				static auto method_UnityObject_getname = il2cpp_symbols_logged::get_method(
					"UnityEngine.CoreModule.dll", "UnityEngine",
					"Object", "get_name", 0
				);
				auto name = (Il2CppString*)reflection::Invoke(method_UnityObject_getname, obj, nullptr, "ExtractAssets|Object::get_name");
				ExtractAsset((Il2CppObject*)item, name);
			}
		}
		else ExtractAsset(obj, name);
	}

	HOOK_ORIG_TYPE AssetBundle_LoadAsset_orig;
	void* AssetBundle_LoadAsset_hook(Il2CppObject* _this, Il2CppString* name, Il2CppReflectionType* type)
	{
		static auto method_AssetBundle_get_name = il2cpp_symbols_logged::get_method("UnityEngine.AssetBundleModule.dll", "UnityEngine", "AssetBundle", "get_name", 0);

		if (g_loadasset_output) {
			auto assetBundleName = method_AssetBundle_get_name->Invoke<Il2CppString*>(_this, {});
			auto cAssetBundleName = assetBundleName->toWstring();
			auto cName = name->toWstring();
			std::wcout << L"[LoadAsset] " << cAssetBundleName << "::" << cName;
			auto klass = il2cpp_class_from_il2cpp_type(type->type);
			std::cout << " : " << klass->namespaze << "::" << klass->name << std::endl;
		}

		auto ret = HOOK_CAST_CALL(void*, AssetBundle_LoadAsset)(_this, name, type);

		if (g_extract_asset) {
			ExtractAssetsGameObject((Il2CppObject*)ret, name);
		}

		return ret;
	}

	class SpanReader {
	private:
		const uint8_t* dataPtr;
		size_t dataSize;
		size_t offset;

	public:
		SpanReader(const uint8_t* data, size_t size) : dataPtr(data), dataSize(size), offset(0) {}

		template<typename T>
		T Read() {
			T value;
			std::memcpy(&value, dataPtr + offset, sizeof(T));
			offset += sizeof(T);

			// if !littleEndian
			std::reverse(reinterpret_cast<uint8_t*>(&value), reinterpret_cast<uint8_t*>(&value) + sizeof(T));

			return value;
		}

		uint64_t ReadUInt64() {
			return Read<uint64_t>();
		}

		uint64_t ReadVarUInt64() {
			uint64_t value = 0;
			int shift = 0;

			do {
				uint8_t b = dataPtr[offset++];
				value |= static_cast<uint64_t>(b & 0x7FU) << shift;
				if (b < 0x80)
					break;
				shift += 7;
			} while (64 > shift);

			return value;
		}
	};

	void* FromCatalogInfo(const std::wstring& encodedInfo, const UINT64 rootHash) {
		static auto CatalogManifest_FromValues = reinterpret_cast<void* (*)(UINT64 labelCrc, UINT64 size, UINT64 checksum, UINT64 seed)>(
			il2cpp_symbols::get_method_pointer("Limelight.Shared.dll", "Limelight",
				"CatalogManifest", "FromValues", 4)
			);

		unsigned char info[5120];
		const auto cvString = utility::conversions::to_utf8string(encodedInfo);
		boost::beast::detail::base64::decode(&info, cvString.c_str(), cvString.size());

		SpanReader reader(info, sizeof(info));

		const auto checksum = reader.ReadUInt64();
		const auto size = reader.ReadVarUInt64();
		const auto seed = reader.ReadUInt64();

		// wprintf(L"encodedInfo: %ls, rootHash: %llu, size: %llu, checksum: %llu, seed: %llu\n", encodedInfo.c_str(), rootHash, size, checksum, seed);
		return CatalogManifest_FromValues(rootHash, size, checksum, seed);
	}

	void* entryAsManifest(void* entry) {
		static auto CatalogManifest_FromValues = reinterpret_cast<void* (*)(UINT64 labelCrc, UINT64 size, UINT64 checksum, UINT64 seed)>(
			il2cpp_symbols::get_method_pointer("Limelight.Shared.dll", "Limelight",
				"CatalogManifest", "FromValues", 4)
			);

		static auto CatalogBinaryEntry_klass = il2cpp_symbols::get_class("Limelight.Shared.dll", "Limelight", "CatalogBinaryEntry");
		static auto LabelCrc_field = il2cpp_class_get_field_from_name(CatalogBinaryEntry_klass, "<LabelCrc>k__BackingField");
		static auto Size_field = il2cpp_class_get_field_from_name(CatalogBinaryEntry_klass, "<Size>k__BackingField");
		static auto Checksum_field = il2cpp_class_get_field_from_name(CatalogBinaryEntry_klass, "<Checksum>k__BackingField");
		static auto Seed_field = il2cpp_class_get_field_from_name(CatalogBinaryEntry_klass, "<Seed>k__BackingField");

		const auto LabelCrc = il2cpp_symbols::read_field<UINT64>(entry, LabelCrc_field);
		const auto Size = il2cpp_symbols::read_field<UINT64>(entry, Size_field);
		const auto Checksum = il2cpp_symbols::read_field<UINT64>(entry, Checksum_field);
		const auto Seed = il2cpp_symbols::read_field<UINT64>(entry, Seed_field);

		return CatalogManifest_FromValues(LabelCrc, Size, Checksum, Seed);
	}


	void* checkAndDownloadFile(Il2CppString* fileNameStr) {
		try {
			const std::wstring fileName(fileNameStr->start_char);

			std::filesystem::path userProfile = getenv("UserProfile");
			std::filesystem::path filePath = userProfile / "AppData/LocalLow/BNE/imasscprism/D" / fileName.substr(0, 2) / fileName;
			if (std::filesystem::exists(filePath)) {
				return readFileAllBytes(filePath);
			}

			const auto url = std::format(L"https://asset.imassc.song4.prism.bn765.com/r/{}/{}", fileName.substr(0, 2), fileName);
			web::http::client::http_client_config cfg;
			cfg.set_timeout(utility::seconds(30));
			web::http::client::http_client client(url, cfg);
			auto response = client.request(web::http::methods::GET).get();
			if (response.status_code() != 200) {
				wprintf(L"File download failed: %ls (%d)\n", url.c_str(), response.status_code());
				return NULL;
			}

			concurrency::streams::fstream::open_ostream(filePath.c_str()).then([=](concurrency::streams::ostream output_stream) {
				return response.body().read_to_end(output_stream.streambuf());
				}).then([=](size_t) {
					wprintf(L"File downloaded successfully: %ls\n", filePath.c_str());
					}).wait();

				return readFileAllBytes(filePath);
		}
		catch (std::exception& e) {
			printf("checkAndDownloadFile error: %s\n", e.what());
			return NULL;
		}

	}

	struct GameVersion {
		std::wstring gameVersion;
		std::wstring resourceVersion;
	};

	GameVersion getGameVersions() {
		static auto getGameVersion = reinterpret_cast<Il2CppString * (*)()>(
			il2cpp_symbols::get_method_pointer(
				"UnityEngine.CoreModule.dll", "UnityEngine",
				"Application", "get_version", 0
			)
			);

		static auto Global_get_instance = reinterpret_cast<void* (*)()>(
			il2cpp_symbols::get_method_pointer("PRISM.ResourceManagement.dll", "PRISM",
				"Global", "get_Instance", 0)
			);

		static auto get_CurrentResourceVersion = reinterpret_cast<Il2CppString * (*)(void*)>(
			il2cpp_symbols::get_method_pointer("PRISM.ResourceManagement.dll", "PRISM",
				"Global", "get_CurrentResourceVersion", 0)
			);

		const std::wstring gameVer(getGameVersion()->start_char);
		const std::wstring resVersion(get_CurrentResourceVersion(Global_get_instance())->start_char);

		return GameVersion{ .gameVersion = gameVer , .resourceVersion = resVersion };
	}

	std::pair<void*, void*> getManifestFile(Il2CppString* defaultFileName) {
		try {
			auto defaultData = checkAndDownloadFile(defaultFileName);
			if (defaultData != NULL) {
				return std::make_pair(nullptr, defaultData);
			}
			printf("Try get fallback version...\n");

			static auto CatalogManifest_FromUniqueVersion = reinterpret_cast<void* (*)(Il2CppString * uniqueVersion, Il2CppString * bundleVersion)>(
				il2cpp_symbols::get_method_pointer("Limelight.Shared.dll", "Limelight",
					"CatalogManifest", "FromUniqueVersion", 2)
				);
			static auto CatalogManifest_GetRealName = reinterpret_cast<Il2CppString * (*)(void*)>(
				il2cpp_symbols::get_method_pointer("Limelight.Shared.dll", "Limelight",
					"CatalogManifest", "GetRealName", 0)
				);

			const auto gameVerInfo = getGameVersions();
			const std::wstring gameVer = gameVerInfo.gameVersion;
			const std::wstring resVersion = gameVerInfo.resourceVersion;

			const auto subPointPos = gameVer.rfind(L'.', -1);
			if (subPointPos == std::wstring::npos) {
				wprintf(L"Invalid game version: %ls\n", gameVer.c_str());
				return std::make_pair(nullptr, nullptr);
			}
			const auto baseVersionString = gameVer.substr(0, subPointPos + 1);
			auto subVersion = std::stoi(gameVer.substr(subPointPos + 1, gameVer.length()));

			while (subVersion >= 0) {
				const auto currentVersion = std::format(L"{}{}", baseVersionString, subVersion);
				wprintf(L"Checking version: %ls\n", currentVersion.c_str());
				auto manifest = CatalogManifest_FromUniqueVersion(il2cpp_symbols::NewWStr(resVersion), il2cpp_symbols::NewWStr(currentVersion));
				if (manifest) {
					auto data = checkAndDownloadFile(CatalogManifest_GetRealName(manifest));
					if (data != NULL) {
						return std::make_pair(manifest, data);
					}
				}
				else {
					printf("manifest is null.\n");
				}
				subVersion--;
			}
		}
		catch (std::exception& ex) {
			printf("getManifestFile error: %s\n", ex.what());
		}
		printf("Get manifest file failed.\n");
		return std::make_pair(nullptr, nullptr);
	}


	int dumpScenarioFromCatalog() {
		static auto string_op_Implicit = reinterpret_cast<void* (*)(void* retstr, Il2CppString*)>(
			il2cpp_symbols::get_method_pointer("mscorlib.dll", "System",
				"String", "op_Implicit", 1)
			);

		static auto ReadOnlySpan_char_klass = il2cpp_symbols::get_system_class_from_reflection_type_str(
			"System.ReadOnlySpan`1[System.Char]");

		static auto HashUtils_Crc64 = reinterpret_cast<UINT64(*)(void*, UINT64)>(
			il2cpp_symbols::get_method_pointer("Limelight.Shared.dll", "Limelight",
				"HashUtils", "Crc64", 2)
			);

		static auto CatalogManifest_GetRealName = reinterpret_cast<Il2CppString * (*)(void*)>(
			il2cpp_symbols::get_method_pointer("Limelight.Shared.dll", "Limelight",
				"CatalogManifest", "GetRealName", 0)
			);

		static auto CatalogBinaryParser_ParseEncoded = reinterpret_cast<void* (*)(void* manifest, void* input)>(
			il2cpp_symbols::get_method_pointer("Limelight.Shared.dll", "Limelight",
				"CatalogBinaryParser", "ParseEncoded", 2)
			);

		static auto CatalogBinary_get_Entries = reinterpret_cast<void* (*)(void*)>(
			il2cpp_symbols::get_method_pointer("Limelight.Shared.dll", "Limelight",
				"CatalogBinary", "get_Entries", 0)
			);

		static auto CatalogBinaryEntry_ToCatalogManifest = reinterpret_cast<void* (*)(void*)>(
			il2cpp_symbols::get_method_pointer("Limelight.Shared.dll", "Limelight",
				"CatalogBinaryEntry", "ToCatalogManifest", 0)
			);

		static auto CatalogBinaryEntry_get_Label = reinterpret_cast<void* (*)(void* retstr, void* _this)>(
			il2cpp_symbols::get_method_pointer("Limelight.Shared.dll", "Limelight",
				"CatalogBinaryEntry", "get_Label", 0)
			);

		static auto CatalogManifest_FromUniqueVersion = reinterpret_cast<void* (*)(Il2CppString * uniqueVersion, Il2CppString * bundleVersion)>(
			il2cpp_symbols::get_method_pointer("Limelight.Shared.dll", "Limelight",
				"CatalogManifest", "FromUniqueVersion", 2)
			);

		static auto CatalogBinaryEntry_klass = il2cpp_symbols::get_class("Limelight.Shared.dll", "Limelight", "CatalogBinaryEntry");
		static auto Label_field = il2cpp_class_get_field_from_name(CatalogBinaryEntry_klass, "<Label>k__BackingField");

		static auto Encoding_get_UTF8 = reinterpret_cast<void* (*)()>(
			il2cpp_symbols::get_method_pointer("mscorlib.dll", "System.Text",
				"Encoding", "get_UTF8", 0)
			);

		static auto Encoding_GetString = reinterpret_cast<Il2CppString * (*)(void*, void*, int, int)>(
			il2cpp_symbols::get_method_pointer("mscorlib.dll", "System.Text",
				"Encoding", "GetString", 3)
			);

		const auto gameVerInfo = getGameVersions();
		const std::wstring gameVer = gameVerInfo.gameVersion;
		const std::wstring resVersion = gameVerInfo.resourceVersion;

		const auto atPos = resVersion.find(L'@');
		const auto simpleVersion = resVersion.substr(0, atPos);
		const auto encodedInfo = resVersion.substr(atPos + 1);

		auto ReadOnlySpan_char = il2cpp_object_new(ReadOnlySpan_char_klass);

		const auto cmpStr = il2cpp_symbols::NewWStr(std::format(L"{}:{}", gameVer, simpleVersion));
		const auto rootHash = HashUtils_Crc64(string_op_Implicit(ReadOnlySpan_char, cmpStr), 0);

		auto manifest = FromCatalogInfo(encodedInfo, rootHash);
		// CatalogManifest_FromUniqueVersion(il2cpp_string_new("10003400@9t5HYphlvDRn0EijwOLgfp0="), il2cpp_string_new("1.7.0"));
		// auto manifest = CatalogManifest_FromUniqueVersion(il2cpp_symbols::NewWStr(resVersion), il2cpp_symbols::NewWStr(gameVer));

		wprintf(L"manifest at %p, rootHash: %llu (%ls), resVersion: %ls\n", manifest, rootHash, cmpStr->start_char, resVersion.c_str());

		auto [newManifest, encodedCatalog] = getManifestFile(CatalogManifest_GetRealName(manifest));
		if (encodedCatalog == nullptr) {
			printf("readCatalog failed: encodedCatalog is NULL.\n");
			return 0;
		}
		if (newManifest != nullptr) {
			manifest = newManifest;
		}

		auto catalog = CatalogBinaryParser_ParseEncoded(manifest, encodedCatalog);  // Limelight.CatalogBinary
		int dumpCount = 0;

		il2cpp_symbols::iterate_IEnumerable(CatalogBinary_get_Entries(catalog), [&dumpCount](void* entry) {
			auto manifest2 = entryAsManifest(entry);
			auto encodedCatalog2 = checkAndDownloadFile(CatalogManifest_GetRealName(manifest2));
			if (encodedCatalog2 == NULL) {
				printf("encodedCatalog2 failed: encodedCatalog is NULL.\n");
				return;
			}
			auto catalog2 = CatalogBinaryParser_ParseEncoded(manifest2, encodedCatalog2);  // Limelight.CatalogBinary

			il2cpp_symbols::iterate_IEnumerable(CatalogBinary_get_Entries(catalog2), [&dumpCount](void* info) {
				static auto toJsonStr = reinterpret_cast<Il2CppString * (*)(void*)>(
					il2cpp_symbols::get_method_pointer("Newtonsoft.Json.dll", "Newtonsoft.Json",
						"JsonConvert", "SerializeObject", 1)
					);

				/*
				wprintf(L"label:\n%ls\n\n", toJsonStr(info)->start_char);
				static auto ArraySegment_Byte_klass = il2cpp_symbols::get_system_class_from_reflection_type_str(
					"System.ArraySegment`1[System.Byte]");
				auto label = il2cpp_object_new(ArraySegment_Byte_klass);  // System.ArraySegment`1<unsigned int8>

				label = CatalogBinaryEntry_get_Label(label, info);  // System.ArraySegment`1<unsigned int8>
				*/

				auto label = il2cpp_field_get_value_object(Label_field, info);

				const std::wstring labelArrayStr(toJsonStr(label)->start_char);
				auto labelArray = nlohmann::json::parse(labelArrayStr);
				std::string labelStr;
				for (auto& i : labelArray) {
					int l = i;
					labelStr += (char)l;
				}

				static std::regex pattern("^s\\d+_\\d+_\\d+$");

				if (std::regex_match(labelStr, pattern)) {
					// printf("labelStr: %s\n", labelStr.c_str());
					if (dumpScenarioDataByJsonFileName(labelStr)) {
						dumpCount++;
					}
				}

				/*
				static auto ArraySegment_Byte_klass = il2cpp_symbols::get_class_from_instance(label);

				static auto ArraySegment_get_Array = reinterpret_cast<void* (*)(void*)>(
					il2cpp_class_get_method_from_name(ArraySegment_Byte_klass, "get_Array", 0)->methodPointer
					);
				static auto ArraySegment_get_Offset = reinterpret_cast<int (*)(void*)>(
					il2cpp_class_get_method_from_name(ArraySegment_Byte_klass, "get_Offset", 0)->methodPointer
					);
				static auto ArraySegment_get_Count = reinterpret_cast<int (*)(void*)>(
					il2cpp_class_get_method_from_name(ArraySegment_Byte_klass, "get_Count", 0)->methodPointer
					);

				auto labelStrArray = ArraySegment_get_Array(label);
				auto labelStrOffset = ArraySegment_get_Offset(label);
				auto labelStrCount = ArraySegment_get_Count(label);


				wprintf(L"labelStrOffset: %d, labelStrCount: %d\n", labelStrOffset, labelStrCount);
				auto labelStr = Encoding_GetString(Encoding_get_UTF8(), labelStrArray, labelStrOffset, labelStrCount);

				wprintf(L"labelStr: %ls\n", labelStr->start_char);*/
				});

			});
		return dumpCount;
	}

	HOOK_ORIG_TYPE LiveMVOverlayView_UpdateLyrics_orig;
	void LiveMVOverlayView_UpdateLyrics_hook(void* _this, Il2CppString* text) {
		const std::wstring origWstr(text->start_char);
		const auto newText = SCLocal::getLyricsTrans(origWstr);
		return HOOK_CAST_CALL(void, LiveMVOverlayView_UpdateLyrics)(_this, il2cpp_string_new(newText.c_str()));
	}

	std::pair<std::wstring, std::string> lastLrc{};
	HOOK_ORIG_TYPE TimelineController_SetLyric_orig;
	void TimelineController_SetLyric_hook(void* _this, Il2CppString* text) {
		const std::wstring origWstr(text->start_char);
		std::string newText = "";
		if (!origWstr.empty()) {
			if (lastLrc.first.compare(origWstr) != 0) {
				newText = SCLocal::getLyricsTrans(origWstr);
				lastLrc.first = origWstr;
				lastLrc.second = newText;
			}
			else {
				newText = lastLrc.second;
			}
		}
		return HOOK_CAST_CALL(void, TimelineController_SetLyric)(_this, il2cpp_string_new(newText.c_str()));
	}

	HOOK_ORIG_TYPE TMP_Text_set_text_orig;
	void TMP_Text_set_text_hook(void* _this, Il2CppString* value) {
		if (needPrintStack) {
			//wprintf(L"TMP_Text_set_text: %ls\n", value->start_char);
			//printf("%ls\n\n", environment_get_stacktrace()->start_char);
		}
		//if(value) value = il2cpp_symbols::NewWStr(std::format(L"(h){}", std::wstring(value->start_char)));
		HOOK_CAST_CALL(void, TMP_Text_set_text)(
			_this, value
			);
	}

	bool get_NeedsLocalization_func(void* instanceUITextMeshProUGUI) {
		return false;
		/*
		// static auto UITextMeshProUGUI_klass = il2cpp_symbols::get_class("PRISM.Legacy.dll", "ENTERPRISE.UI", "UITextMeshProUGUI");
		auto this_klass = il2cpp_symbols::get_class_from_instance(instanceUITextMeshProUGUI);
		auto m_EnvMapMatrix_field = il2cpp_class_get_field_from_name(this_klass, "m_EnvMapMatrix");
		auto Matrix4x4_value_field = il2cpp_symbols::get_field("UnityEngine.CoreModule.dll", "UnityEngine", "Matrix4x4", "m32");

		auto m_EnvMapMatrix = il2cpp_symbols::read_field(instanceUITextMeshProUGUI, m_EnvMapMatrix_field);

		printf("%p, m_EnvMapMatrix_field: %p, m_EnvMapMatrix at %p\n", instanceUITextMeshProUGUI, m_EnvMapMatrix_field, m_EnvMapMatrix);

		if (!m_EnvMapMatrix) return false;

		auto m_EnvMapMatrix_klass = reinterpret_cast<Il2CppClassHead*>(il2cpp_symbols::get_class_from_instance(m_EnvMapMatrix));
		printf("m_EnvMapMatrix class: %s::%s\n", m_EnvMapMatrix_klass->namespaze, m_EnvMapMatrix_klass->name);

		auto Matrix4x4_value = il2cpp_symbols::read_field(m_EnvMapMatrix, Matrix4x4_value_field);
		if (!Matrix4x4_value) return false;

		// printf("m_EnvMapMatrix(%p) - m32: %p\n", m_EnvMapMatrix, Matrix4x4_value);

		return true;*/
	}

	void* (*TMP_FontAsset_CreateFontAsset)(void* font);
	void (*TMP_Text_set_font)(void* _this, void* fontAsset);
	void* (*TMP_Text_get_font)(void* _this);
	// void* lastUpdateFontPtr = nullptr;
	std::unordered_set<void*> updatedFontPtrs{};

	HOOK_ORIG_TYPE UITextMeshProUGUI_Awake_orig;
	void UITextMeshProUGUI_Awake_hook(void* _this) {
		static auto get_Text = reinterpret_cast<Il2CppString * (*)(void*)>(
			il2cpp_symbols::get_method_pointer(
				"PRISM.Legacy.dll", "ENTERPRISE.UI",
				"UITextMeshProUGUI", "get_text", 0
			)
			);
		static auto set_Text = reinterpret_cast<void (*)(void*, Il2CppString*)>(
			il2cpp_symbols::get_method_pointer(
				"PRISM.Legacy.dll", "ENTERPRISE.UI",
				"UITextMeshProUGUI", "set_text", 1
			)
			);

		auto origText = get_Text(_this);
		if (origText) {
			// wprintf(L"UITextMeshProUGUI_Awake: %ls\n", origText->start_char);
			if (!get_NeedsLocalization_func(_this)) {
				std::string newTrans("");
				if (SCLocal::getGameUnlocalTrans(std::wstring(origText->start_char), &newTrans)) {
					set_Text(_this, il2cpp_string_new(newTrans.c_str()));
				}
			}
			else {
				// set_Text(_this, il2cpp_symbols::NewWStr(std::format(L"(接口l){}", std::wstring(origText->start_char))));
			}
		}

		static auto set_sourceFontFile = reinterpret_cast<void (*)(void*, void* font)>(
			il2cpp_symbols::get_method_pointer("Unity.TextMeshPro.dll", "TMPro",
				"TMP_FontAsset", "set_sourceFontFile", 1)
			);
		static auto UpdateFontAssetData = reinterpret_cast<void (*)(void*)>(
			il2cpp_symbols::get_method_pointer("Unity.TextMeshPro.dll", "TMPro",
				"TMP_FontAsset", "UpdateFontAssetData", 0)
			);
		static auto set_fontSize = reinterpret_cast<void (*)(void*, float)>(
			il2cpp_symbols::get_method_pointer("Unity.TextMeshPro.dll", "TMPro",
				"TMP_Text", "set_fontSize", 1)
			);
		static auto get_fontSize = reinterpret_cast<float (*)(void*)>(
			il2cpp_symbols::get_method_pointer("Unity.TextMeshPro.dll", "TMPro",
				"TMP_Text", "get_fontSize", 0)
			);

		auto replaceFont = getReplaceFont();
		if (replaceFont) {
			auto origFont = TMP_Text_get_font(_this);
			set_sourceFontFile(origFont, replaceFont);
			if (!updatedFontPtrs.contains(origFont)) {
				updatedFontPtrs.emplace(origFont);
				UpdateFontAssetData(origFont);
			}
			/*
			if (origFont != lastUpdateFontPtr) {
				UpdateFontAssetData(origFont);
				lastUpdateFontPtr = origFont;
			}*/
		}
		set_fontSize(_this, get_fontSize(_this) + g_font_size_offset);
		HOOK_CAST_CALL(void, UITextMeshProUGUI_Awake)(_this);
	}

	HOOK_ORIG_TYPE ScenarioManager_Init_orig;
	void* ScenarioManager_Init_hook(void* retstr, void* _this, Il2CppString* scrName) {
		// printf("ScenarioManager_Init: %ls\n%ls\n\n", scrName->start_char, environment_get_stacktrace()->start_char);
		return HOOK_CAST_CALL(void*, ScenarioManager_Init)(retstr, _this, scrName);
	}

	void* DataFile_GetBytes_hook(Il2CppString* path) {
		std::wstring pathStr(path->start_char);
		std::wstring_view pathStrView(pathStr);
		//wprintf(L"DataFile_GetBytes: %ls\n", pathStr.c_str());


		std::filesystem::path localFileName;
		if (SCLocal::getLocalFileName(pathStr, &localFileName)) {
			return readFileAllBytes(localFileName);
		}

		auto ret = HOOK_CAST_CALL(void*, DataFile_GetBytes)(path);
		if (g_auto_dump_all_json) {
			if (pathStrView.ends_with(L".json")) {
				auto basePath = std::filesystem::path("dumps") / "autoDumpJson";
				std::filesystem::path fileName = basePath / SCLocal::getFilePathByName(pathStr, true, basePath);

				if (!std::filesystem::is_directory(basePath)) {
					std::filesystem::create_directories(basePath);
				}
				auto newStr = bytesToIl2cppString(ret);
				std::ofstream fileDump(fileName, std::ofstream::out);
				auto writeWstr = std::wstring(newStr->start_char);

				const std::wstring searchStr = L"\r";
				const std::wstring replaceStr = L"\\r";
				size_t pos = writeWstr.find(searchStr);
				while (pos != std::wstring::npos) {
					writeWstr.replace(pos, 1, replaceStr);
					pos = writeWstr.find(searchStr, pos + replaceStr.length());
				}

				fileDump << utility::conversions::to_utf8string(writeWstr).c_str();
				fileDump.close();
				printf("Auto dump: %ls\n", fileName.c_str());
			}
		}
		return ret;
	}

	bool isFirstTimeSetResolution = true;
	void SetResolution_hook(int width, int height, bool fullscreen) {
		// printf("SetResolution: %d, %d, %d, isFirstTimeSetResolution: %d\n", width, height, fullscreen, isFirstTimeSetResolution);
		if (isFirstTimeSetResolution) {
			isFirstTimeSetResolution = false;
			if ((g_start_resolution_w < 0) || (g_start_resolution_h < 0)) {
				g_start_resolution_w = width;
				g_start_resolution_h = height;
				g_start_resolution_fullScreen = fullscreen;
			}
			return HOOK_CAST_CALL(void, SetResolution)(g_start_resolution_w,
				g_start_resolution_h, g_start_resolution_fullScreen);
		}
		return;
		/*
		width = SCGUIData::screenW;
		height = SCGUIData::screenH;
		fullscreen = SCGUIData::screenFull;
		return HOOK_CAST_CALL(void, SetResolution)(width, height, fullscreen);
		*/
	}

	HOOK_ORIG_TYPE InvokeMoveNext_orig;
	void InvokeMoveNext_hook(void* enumerator, void* returnValueAddress) {
		MHotkey::start_hotkey(hotKey);
		return HOOK_CAST_CALL(void, InvokeMoveNext)(enumerator, returnValueAddress);
	}

	// Normal 3D Live
	HOOK_ORIG_TYPE DepthOfFieldClip_CreatePlayable_orig;
	void* DepthOfFieldClip_CreatePlayable_hook(void* retstr, void* _this, void* graph, void* go, void* mtd) {
		if (g_enable_free_camera) {
			static auto DepthOfFieldClip_klass = il2cpp_symbols::get_class("PRISM.Legacy.dll", "PRISM", "DepthOfFieldClip");
			static auto DepthOfFieldClip_behaviour_field = il2cpp_class_get_field_from_name(DepthOfFieldClip_klass, "behaviour");

			static auto DepthOfFieldBehaviour_klass = il2cpp_symbols::get_class("PRISM.Legacy.dll", "PRISM", "DepthOfFieldBehaviour");
			static auto DepthOfFieldBehaviour_focusDistance_field = il2cpp_class_get_field_from_name(DepthOfFieldBehaviour_klass, "focusDistance");
			static auto DepthOfFieldBehaviour_aperture_field = il2cpp_class_get_field_from_name(DepthOfFieldBehaviour_klass, "aperture");
			static auto DepthOfFieldBehaviour_focalLength_field = il2cpp_class_get_field_from_name(DepthOfFieldBehaviour_klass, "focalLength");
			auto depthOfFieldBehaviour = il2cpp_symbols::read_field(_this, DepthOfFieldClip_behaviour_field);
			/*
			auto focusDistance = il2cpp_symbols::read_field<float>(depthOfFieldBehaviour, DepthOfFieldBehaviour_focusDistance_field);
			auto aperture = il2cpp_symbols::read_field<float>(depthOfFieldBehaviour, DepthOfFieldBehaviour_aperture_field);
			auto focalLength = il2cpp_symbols::read_field<float>(depthOfFieldBehaviour, DepthOfFieldBehaviour_focalLength_field);
			*/

			// invalid values from game version v2.6.1 and crash at the original call; bypass temporarily
			// il2cpp_symbols::write_field(depthOfFieldBehaviour, DepthOfFieldBehaviour_focusDistance_field, 1000.0f);
			// il2cpp_symbols::write_field(depthOfFieldBehaviour, DepthOfFieldBehaviour_aperture_field, 32.0f);
			// il2cpp_symbols::write_field(depthOfFieldBehaviour, DepthOfFieldBehaviour_focalLength_field, 1.0f);

			// printf("DepthOfFieldClip_CreatePlayable, focusDistance: %f, aperture: %f, focalLength: %f\n", focusDistance, aperture, focalLength);
		}
		return HOOK_CAST_CALL(void*, DepthOfFieldClip_CreatePlayable)(retstr, _this, graph, go, mtd);
	}

	// obsolete hook removed
	// HDR Live
	//HOOK_ORIG_TYPE PostProcess_DepthOfFieldClip_CreatePlayable_orig;
	//void PostProcess_DepthOfFieldClip_CreatePlayable_hook(void* retstr, void* _this, void* graph, void* go, void* mtd) {

	//	if (g_enable_free_camera) {
	//		static auto DepthOfFieldClip_klass = il2cpp_symbols::get_class("PRISM.Legacy.dll", "UnityEngine.Rendering.Universal.PostProcess", "DepthOfFieldClip");
	//		static auto DepthOfFieldClip_behaviour_field = il2cpp_class_get_field_from_name(DepthOfFieldClip_klass, "behaviour");

	//		static auto DepthOfFieldBehaviour_klass = il2cpp_symbols::get_class("PRISM.Legacy.dll", "UnityEngine.Rendering.Universal.PostProcess", "DepthOfFieldBehaviour");
	//		static auto DepthOfFieldBehaviour_focusDistance_field = il2cpp_class_get_field_from_name(DepthOfFieldBehaviour_klass, "focusDistance");
	//		static auto DepthOfFieldBehaviour_aperture_field = il2cpp_class_get_field_from_name(DepthOfFieldBehaviour_klass, "aperture");
	//		static auto DepthOfFieldBehaviour_focalLength_field = il2cpp_class_get_field_from_name(DepthOfFieldBehaviour_klass, "focalLength");
	//		auto depthOfFieldBehaviour = il2cpp_symbols::read_field(_this, DepthOfFieldClip_behaviour_field);
	//		/*
	//		auto focusDistance = il2cpp_symbols::read_field<float>(depthOfFieldBehaviour, DepthOfFieldBehaviour_focusDistance_field);
	//		auto aperture = il2cpp_symbols::read_field<float>(depthOfFieldBehaviour, DepthOfFieldBehaviour_aperture_field);
	//		auto focalLength = il2cpp_symbols::read_field<float>(depthOfFieldBehaviour, DepthOfFieldBehaviour_focalLength_field);
	//		*/

	//		il2cpp_symbols::write_field(depthOfFieldBehaviour, DepthOfFieldBehaviour_focusDistance_field, 1000.0f);
	//		il2cpp_symbols::write_field(depthOfFieldBehaviour, DepthOfFieldBehaviour_aperture_field, 32.0f);
	//		il2cpp_symbols::write_field(depthOfFieldBehaviour, DepthOfFieldBehaviour_focalLength_field, 1.0f);

	//		// printf("DepthOfFieldClip_CreatePlayable, focusDistance: %f, aperture: %f, focalLength: %f\n", focusDistance, aperture, focalLength);
	//	}

	//	HOOK_CAST_CALL(void, PostProcess_DepthOfFieldClip_CreatePlayable)(retstr, _this, graph, go, mtd);
	//}

	// 已过�?
	HOOK_ORIG_TYPE Live_SetEnableDepthOfField_orig;
	void Live_SetEnableDepthOfField_hook(void* _this, bool isEnable) {
		if (g_enable_free_camera) {
			isEnable = false;
		}
		return HOOK_CAST_CALL(void, Live_SetEnableDepthOfField)(_this, isEnable);
	}

	// 未hook
	HOOK_ORIG_TYPE Live_Update_orig;
	void Live_Update_hook(void* _this) {
		HOOK_CAST_CALL(void, Live_Update)(_this);
		if (g_enable_free_camera) {
			// Live_SetEnableDepthOfField_hook(_this, false);
		}
	}

	// [PATCH] __OBSOLETE_HOOK__ guard removed - this block declares the globals
	// (confirmationingModel / cacheDressMap / cacheHairMap / cacheAccessoryMap) that the
	// costume-unlock code further below needs.
	bool isCancelTryOn = false;
	HOOK_ORIG_TYPE LiveCostumeChangeView_setTryOnMode_orig;
	void LiveCostumeChangeView_setTryOnMode_hook(void* _this, void* idol, bool isTryOn) {
		if (!isTryOn) isCancelTryOn = true;
		HOOK_CAST_CALL(void, LiveCostumeChangeView_setTryOnMode)(_this, idol, isTryOn);
		isCancelTryOn = false;
	}

	HOOK_ORIG_TYPE LiveCostumeChangeView_setIdolCostume_orig;
	void LiveCostumeChangeView_setIdolCostume_hook(void* _this, void* idol, int category, int costumeId) {
		if (g_allow_use_tryon_costume) {
			static auto iidol_klass = il2cpp_symbols::get_class_from_instance(idol);
			static auto get_CharacterId_mtd = il2cpp_class_get_method_from_name(iidol_klass, "get_CharacterId", 0);
			if (get_CharacterId_mtd) {
				const auto idolId = reinterpret_cast<int (*)(void*)>(get_CharacterId_mtd->methodPointer)(idol);
				printf("LiveCostumeChangeView_setIdolCostume idol: %d, category: %d, costumeId: %d\n", idolId, category, costumeId);
			}
			if (isCancelTryOn) return;
		}

		return HOOK_CAST_CALL(void, LiveCostumeChangeView_setIdolCostume)(_this, idol, category, costumeId);
	}

	void* (*LiveCostumeChangeModel_get_Idol)(void*);

#define replaceResourceId(curr) \
	const auto idol = LiveCostumeChangeModel_get_Idol(_this);\
	static auto iidol_klass = il2cpp_symbols::get_class_from_instance(idol);\
	static auto get_CharacterId_mtd = il2cpp_class_get_method_from_name(iidol_klass, "get_CharacterId", 0);\
	const auto idolId = reinterpret_cast<int (*)(void*)>(get_CharacterId_mtd->methodPointer)(idol);\
	const auto resId = il2cpp_symbols::read_field<int>(ret, resourceId_field);\
	printf(#curr"_orig idol: %d, dressId: %d, ResId: %d, this at %p\n", idolId, id, resId, _this);\
	\
	int replaceDressId = baseId + idolId * 1000;\
	\
	auto replaceResId = id == replaceDressId ? baseResId : il2cpp_symbols::read_field<int>(ret, resourceId_field);\
	auto newRet = HOOK_CAST_CALL(void*, curr)(_this, replaceDressId);\
	if (!newRet) {\
		printf("ReplaceId: %d not found. try++\n", replaceDressId);\
		replaceDressId++;\
		newRet = HOOK_CAST_CALL(void*, curr)(_this, replaceDressId);\
		if (!newRet){\
			printf("replaceId: %d not found. Replace failed.\n", replaceDressId);\
			return ret;\
		}\
		replaceResId = id == replaceDressId ? baseResId : il2cpp_symbols::read_field<int>(ret, resourceId_field);\
	}\
	il2cpp_symbols::write_field(newRet, resourceId_field, replaceResId)


	HOOK_ORIG_TYPE LiveCostumeChangeModel_GetDress_orig;
	void* LiveCostumeChangeModel_GetDress_hook(void* _this, int id) {  // 替换服装 ResID
		auto ret = HOOK_CAST_CALL(void*, LiveCostumeChangeModel_GetDress)(_this, id);
		if (!g_unlock_all_dress) return ret;
		if (!ret) {
			printf("LiveCostumeChangeModel_GetDress returns NULL ResId: %d, this at %p\n", id, _this);
			return ret;
		}
		static auto ret_klass = il2cpp_symbols::get_class_from_instance(ret);
		static auto resourceId_field = il2cpp_class_get_field_from_name(ret_klass, "resourceId_");

		const auto baseId = 100001;
		const auto baseResId = 1;
		replaceResourceId(LiveCostumeChangeModel_GetDress);
		return newRet;
	}

	HOOK_ORIG_TYPE LiveCostumeChangeModel_GetAccessory_orig;
	void* LiveCostumeChangeModel_GetAccessory_hook(void* _this, int id) {  // 替换饰品 ResID
		auto ret = HOOK_CAST_CALL(void*, LiveCostumeChangeModel_GetAccessory)(_this, id);
		if (!(g_unlock_all_dress && g_unlock_all_headwear)) return ret;
		if (!ret) {
			printf("LiveCostumeChangeModel_GetAccessory returns NULL ResId: %d, this at %p\n", id, _this);
			return ret;
		}
		static auto ret_klass = il2cpp_symbols::get_class_from_instance(ret);
		static auto resourceId_field = il2cpp_class_get_field_from_name(ret_klass, "resourceId_");
		static auto accessoryType_field = il2cpp_class_get_field_from_name(ret_klass, "accessoryType_");

		const auto accessoryType = il2cpp_symbols::read_field<int>(ret, accessoryType_field);
		printf("accessoryType: %d\n", accessoryType);
		int baseId;
		int baseResId;
		switch (accessoryType) {
		case 0x1: {  // Glasses
			baseId = 800006;
			baseResId = 1103400;
		}; break;
		case 0x2: {  // Earrings
			baseId = 800001;
			baseResId = 2101300;
		}; break;
		case 0x3: {  // Makeup
			baseId = 800009;
			baseResId = 9100103;
		}; break;
		default: return ret;
		}

		replaceResourceId(LiveCostumeChangeModel_GetAccessory);
		// LiveCostumeChangeModel_GetAccessory_hook idol : 15, dressId : 815004, ResId : 1103200, this at 0000021E62ADDD80
		return newRet;
	}

	HOOK_ORIG_TYPE LiveCostumeChangeModel_GetHairstyle_orig;
	void* LiveCostumeChangeModel_GetHairstyle_hook(void* _this, int id) {  // 替换头发 ResID
		auto ret = HOOK_CAST_CALL(void*, LiveCostumeChangeModel_GetHairstyle)(_this, id);
		if (!(g_unlock_all_dress && g_unlock_all_headwear)) return ret;
		if (!ret) {
			printf("LiveCostumeChangeModel_GetHairstyle returns NULL ResId: %d, this at %p\n", id, _this);
			return ret;
		}
		static auto ret_klass = il2cpp_symbols::get_class_from_instance(ret);
		static auto resourceId_field = il2cpp_class_get_field_from_name(ret_klass, "resourceId_");

		const auto baseId = 600001;
		const auto baseResId = 1;
		replaceResourceId(LiveCostumeChangeModel_GetHairstyle);

		static auto accessoryResourceIdList_field = il2cpp_class_get_field_from_name(ret_klass, "accessoryResourceIdList_");
		const auto replaceaccessoryResourceIdList = il2cpp_symbols::read_field(ret, accessoryResourceIdList_field);
		if (id == replaceDressId) {
			static auto RepeatedField_klass = il2cpp_symbols::get_class_from_instance(replaceaccessoryResourceIdList);
			static auto RepeatedField_Add_mtd = il2cpp_class_get_method_from_name(RepeatedField_klass, "Add", 1);
			static auto RepeatedField_Add = reinterpret_cast<void (*)(void*, int, void* mtd)>(RepeatedField_Add_mtd->methodPointer);
			static auto RepeatedField_ctor_mtd = il2cpp_class_get_method_from_name(RepeatedField_klass, ".ctor", 0);
			static auto RepeatedField_ctor = reinterpret_cast<void (*)(void*, void* mtd)>(RepeatedField_ctor_mtd->methodPointer);

			auto newRepeatedField = il2cpp_object_new(RepeatedField_klass);
			RepeatedField_ctor(newRepeatedField, RepeatedField_ctor_mtd);
			RepeatedField_Add(newRepeatedField, 0, RepeatedField_ctor_mtd);
			RepeatedField_Add(newRepeatedField, 0, RepeatedField_ctor_mtd);
			il2cpp_symbols::write_field(newRet, accessoryResourceIdList_field, newRepeatedField);
		}
		else {
			il2cpp_symbols::write_field(newRet, accessoryResourceIdList_field, replaceaccessoryResourceIdList);
		}

		return newRet;
	}

	bool confirmationingModel = false;
	std::map<int, void*> cacheDressMap{};
	std::map<int, void*> cacheHairMap{};
	std::map<int, void*> cacheAccessoryMap{};

	HOOK_ORIG_TYPE LiveCostumeChangeModel_ctor_orig;
	void LiveCostumeChangeModel_ctor_hook(void* _this, void* reply, void* idol, int costumeType, bool forceDressOrdered) {  // 添加服装�?dressDic
		/*
		static auto iidol_klass = il2cpp_symbols::get_class_from_instance(idol);
		static auto get_CharacterId_mtd = il2cpp_class_get_method_from_name(iidol_klass, "get_CharacterId", 0);
		if (get_CharacterId_mtd) {
			const auto idolId = reinterpret_cast<int (*)(void*)>(get_CharacterId_mtd->methodPointer)(idol);
			printf("LiveCostumeChangeModel_ctor idolId: %d, costumeType: %d\n", idolId, costumeType);
		}
		*/
		HOOK_CAST_CALL(void, LiveCostumeChangeModel_ctor)(_this, reply, idol, costumeType, forceDressOrdered);

		if (g_unlock_all_dress && (costumeType == 1)) {
			static auto LiveCostumeChangeModel_klass = il2cpp_symbols::get_class("PRISM.Adapters.dll", "PRISM.Adapters", "LiveCostumeChangeModel");
			static auto dressDic_field = il2cpp_class_get_field_from_name(LiveCostumeChangeModel_klass, "dressDic");
			static auto hairstyleDic_field = il2cpp_class_get_field_from_name(LiveCostumeChangeModel_klass, "hairstyleDic");
			static auto accessoryDic_field = il2cpp_class_get_field_from_name(LiveCostumeChangeModel_klass, "accessoryDic");

			auto dressDicInstance = il2cpp_field_get_value_object(dressDic_field, _this);
			// auto hairstyleDicInstance = il2cpp_field_get_value_object(hairstyleDic_field, _this);
			// auto accessoryDicInstance = il2cpp_field_get_value_object(accessoryDic_field, _this);

			auto dressDic = Utils::CSDictEditor<int>(dressDicInstance, "System.Collections.Generic.Dictionary`2[System.Int32, PRISM.Module.Networking.ICostumeStatus]");
			// auto hairstyleDic = Utils::CSDictEditor<int>(hairstyleDicInstance, "System.Collections.Generic.Dictionary`2[System.Int32, PRISM.Module.Networking.IHairstyleStatus]");
			// auto accessoryDic = Utils::CSDictEditor<int>(accessoryDicInstance, "System.Collections.Generic.Dictionary`2[System.Int32, PRISM.Module.Networking.IAccessoryStatus]");

			const auto addToDic = [](std::map<int, void*>& cacheMap, Utils::CSDictEditor<int>& dict) {
				for (auto& i : cacheMap) {
					const auto currId = i.first;
					if (!dict.ContainsKey(currId)) {
						auto& costumeStat = i.second;
						// printf("add: %p\n", costumeStat);
						dict.Add(currId, costumeStat);
					}
				}};

			addToDic(cacheDressMap, dressDic);
			// addToDic(cacheHairMap, hairstyleDic);  // TODO PRISM.ResourceManagement.ResourceLoader._throwMissingKeyException
			// addToDic(cacheAccessoryMap, accessoryDic);  // TODO PRISM.ResourceManagement.ResourceLoader._throwMissingKeyException
		}
	}
	// [PATCH] #endif removed (matching the #ifdef __OBSOLETE_HOOK__ that was deleted above)


	// [DIAG] v32 - solo index mapping experiment.  The ctor gets the index of
	// the selected character in the *pre-sort* lineup, but the vocal stem /
	// post-load lineup may use another order.  Log the candidate orders and
	// optionally write the mapped index back into the LiveMVStartData field.
	namespace SoloUnlock {
		int ReadSongIdFromMvStage(void* mvStage);
		std::string ReadMvSceneFromMvStage(void* mvStage);
		void DumpSongPartsForSong(int songId);
		void ApplySoloIndexMap(void* _this, void* onStageIdols, int origIndex, int charaId, int songId);
		void RememberStartData(void* data, int origIndex, int charaId, int songId);
		void RemapAtPreLoad(void* data, void* onStageIdols);
	}

	int g_lastSoloVocalMode = -1;
	int g_lastSoloVocalIndex = -1;
	int g_lastSoloIsSortIdols = 0;
	int g_soloPositionLogCalls = 0;

	void ModifyOnStageIdols(void* onStageIdols) {
		FILE* soloLog = nullptr;
		__try {
			auto idolsLength = il2cpp_array_length(onStageIdols);
			if (g_soloPositionLogCalls++ < 100) {
				soloLog = fopen("solo-unlock.txt", "a");
				if (soloLog) {
					fprintf(soloLog, "[v31] lineup mode=%d soloIndex=%d sortIdols=%d positions=[",
						g_lastSoloVocalMode, g_lastSoloVocalIndex, g_lastSoloIsSortIdols);
					for (int i = 0; i < idolsLength && i < 32; ++i) {
						auto idol = (managed::UnitIdol*)il2cpp_symbols::array_get_value(onStageIdols, i);
						fprintf(soloLog, "%s%d:%d", i ? "," : "", i,
							idol ? UnitIdol::GetCharaId(idol) : -1);
					}
					fprintf(soloLog, "]\n");
					fclose(soloLog);
					soloLog = nullptr;
				}
			}
			if (g_save_and_replace_costume_changes) {
				for (int i = 0; i < idolsLength; i++) {
					auto item = (managed::UnitIdol*)il2cpp_symbols::array_get_value(onStageIdols, i);
					auto it = savedCostumes.find(UnitIdol::GetCharaId(item));
					if (it != savedCostumes.end()) {
						it->second.ApplyTo(item, true);
						std::cout << "CharaId " << it->first << " has been modified." << std::endl;
					}
				}
			}
			if (g_overrie_mv_unit_idols) {
				auto loopMax = idolsLength;
				if (idolsLength > overridenMvUnitIdols_length) {
					printf("[WARNING]: `onStageIdols.Length` = %d, greater than expected `overridenMvUnitIdols_length`.\n", idolsLength);
					loopMax = overridenMvUnitIdols_length;
				}
				for (int i = 0; i < loopMax; ++i) {
					if (!overridenMvUnitIdols[i].IsEmpty()) {
						auto item = (managed::UnitIdol*)il2cpp_symbols::array_get_value(onStageIdols, i);
						overridenMvUnitIdols[i].ApplyTo(item, true);
						printf("MV unit idol [%d] is overriden.\n", i);
					}
				}
			}
		}
		__except (seh_filter(GetExceptionInformation())) {
			if (soloLog) fclose(soloLog);
			printf("SEH exception detected in `ModifyOnStageIdols`.\n");
		}
	}

	HOOK_ORIG_TYPE LiveMVStartData_ctor_orig;
	void LiveMVStartData_ctor_hook(void* _this, void* mvStage, void* sceneName, void* onStageIdols, void* cameraworkConfig, int vocalSeparatedMode, int vocalSeparatedSoloIndex, int renderingDynamicRange, int soundEffectMode, bool isSortIdols) {
		// [DIAG] v31 - scalar-only logging; do not inspect managed arguments here.
		g_lastSoloVocalMode = vocalSeparatedMode;
		g_lastSoloVocalIndex = vocalSeparatedSoloIndex;
		g_lastSoloIsSortIdols = isSortIdols ? 1 : 0;

		int selectedCharaId = -1;
		int mvSongId = -1;
		std::string mvScene;
		try { mvSongId = SoloUnlock::ReadSongIdFromMvStage(mvStage); } catch (...) {}
		try { mvScene = SoloUnlock::ReadMvSceneFromMvStage(mvStage); } catch (...) {}
		try {
			auto idolsLength = onStageIdols ? il2cpp_array_length(onStageIdols) : 0;
			if (onStageIdols && vocalSeparatedSoloIndex >= 0 && (uint32_t)vocalSeparatedSoloIndex < idolsLength) {
				auto idol = (managed::UnitIdol*)il2cpp_symbols::array_get_value(onStageIdols, vocalSeparatedSoloIndex);
				if (idol) selectedCharaId = UnitIdol::GetCharaId(idol);
			}
		} catch (...) {}
		try { SoloUnlock::DumpSongPartsForSong(mvSongId); } catch (...) {}
		try { SoloUnlock::RememberStartData(_this, vocalSeparatedSoloIndex, selectedCharaId, mvSongId); } catch (...) {}

		static int soloStartDataLogs = 0;
		if (soloStartDataLogs++ < 100) {
			FILE* f = fopen("solo-unlock.txt", "a");
			if (f) {
				fprintf(f, "[v31] LiveMVStartData mode=%d soloIndex=%d sortIdols=%d mvStage=%p sceneName=%p onStageIdols=%p camerawork=%p\n",
					vocalSeparatedMode, vocalSeparatedSoloIndex, isSortIdols ? 1 : 0,
					mvStage, sceneName, onStageIdols, cameraworkConfig);
				fprintf(f, "[v32] start song=%d scene=\"%s\" mode=%d soloIndex=%d chara=%d\n",
					mvSongId, mvScene.c_str(), vocalSeparatedMode, vocalSeparatedSoloIndex, selectedCharaId);
				fclose(f);
			}
		}
		if (g_override_isVocalSeparatedOn) {
			vocalSeparatedMode = 1;
		}
		HOOK_CAST_CALL(void*, LiveMVStartData_ctor)(_this, mvStage, sceneName, onStageIdols, cameraworkConfig, vocalSeparatedMode, vocalSeparatedSoloIndex, renderingDynamicRange, soundEffectMode, isSortIdols);
		if (selectedCharaId >= 0) {
			try { SoloUnlock::ApplySoloIndexMap(_this, onStageIdols, vocalSeparatedSoloIndex, selectedCharaId, mvSongId); } catch (...) {}
		}
	}

	void* GetLiveStartDataOnStageIdols(Il2CppObject* data) {
		if (data == nullptr) return nullptr;
		auto method_get_OnStageIdols = il2cpp_symbols_logged::get_method(
			il2cpp_object_get_class(data), "get_OnStageIdols", 0
		);
		return method_get_OnStageIdols->Invoke(data, {});
	}
	Il2CppObject* CreateRunwayMotionInstanceOrDefault(int id) {
		if (id <= 0) return nullptr;

		// IAdapterLocator locator = IAdapterLocator.Instance;
		static auto method_IAdapterLocator_get_Instance = il2cpp_symbols_logged::get_method(
			"PRISM.Adapters.dll", "PRISM.Adapters",
			"IAdapterLocator", "get_Instance", 0);
		auto locator = method_IAdapterLocator_get_Instance->Invoke<Il2CppObject*>(nullptr, {});
		auto locatorType = il2cpp_symbols::get_class_from_instance(locator);

		// MasterData masterData = locator.MasterData;
		auto method_locatorType_get_MasterData = il2cpp_class_get_method_from_name(
			locatorType, "get_MasterData", 0);
		auto masterData = method_locatorType_get_MasterData
			->Invoke<Il2CppObject*>(locator, {});

		// SortedMasterTable<int, MstRunwayMotion> runwayMotions = masterData.RunwayMotions;
		static auto klass_MasterData = il2cpp_symbols_logged::get_class(
			"PRISM.Definitions.dll", "PRISM.Definitions", "MasterData");
		static auto field_MasterData_RunwayMotions =
			il2cpp_symbols_logged::il2cpp_class_get_field_from_name(
				klass_MasterData, "RunwayMotions");
		auto runwayMotions = il2cpp_symbols::read_field<Il2CppObject*>(
			masterData, field_MasterData_RunwayMotions);

		// var motion = runwayMotions.GetValueOrDefault(id);
		// if (motion is null) { Console.WriteLine(...); }
		// return motion;
		auto method_runwayMotions_GetValueOrDefault =
			il2cpp_class_get_method_from_name(il2cpp_symbols::get_class_from_instance(
				runwayMotions), "GetValueOrDefault", 1);
		auto motion = method_runwayMotions_GetValueOrDefault
			->Invoke<Il2CppObject*>(runwayMotions, { (Il2CppObject*)&id }
			);
		if (!motion) {
			printf("[Error] Failed to resolve motion id '%d' from MasterData.", id);
		}
		return motion;
	}

	void OverrideRunwayStartData(
		Il2CppObject* runwayEventStartData, int position, int walkingId, int poseId
	) {
		// RunwayEventMotionConfig config = runwayEventStartData.MotionConfig;
		static auto method_RunwayEventStartData_get_MotionConfig = il2cpp_symbols_logged::get_method(
			"PRISM.Legacy.dll", "PRISM.RunwayEvent",
			"RunwayEventStartData", "get_MotionConfig", 0);
		auto config = method_RunwayEventStartData_get_MotionConfig
			->Invoke<Il2CppObject*>(runwayEventStartData, {});

		// IReadOnlyList<RunwayEventMotion> motions = config.Motions;
		static auto method_RunwayEventMotionConfig_get_Motions = il2cpp_symbols_logged::get_method(
			"PRISM.Legacy.dll", "PRISM.RunwayEvent",
			"RunwayEventMotionConfig", "get_Motions", 0);
		auto motions = method_RunwayEventMotionConfig_get_Motions
			->Invoke<Il2CppObject*>(config, {});

		// MstRunwayMotion walking = ::CreateRunwayMotionInstanceOrDefault(walkingId);
		auto walking = CreateRunwayMotionInstanceOrDefault(walkingId);
		// if (!walking.IsWalkingPattern) {
		//     Console.WriteLine(...);
		//     walking = null;
		// }
		static auto method_MstRunwayMotion_get_IsWalkingPattern =
			il2cpp_symbols_logged::get_method(
				"PRISM.Definitions.dll", "PRISM.Definitions",
				"MstRunwayMotion", "get_IsWalkingPattern", 0);
		auto isWalkingPattern = method_MstRunwayMotion_get_IsWalkingPattern
			->Invoke((Il2CppObject*)walking, {})
			->unbox_value<bool>();
		if (!isWalkingPattern) {
			std::cout << "[OverrideRunwayStartData] Motion id '" << walkingId << "' isn't a walking. Keeping unchanged." << std::endl;
			walking = nullptr;
		}

		// MstRunwayMotion pose = ::CreateRunwayMotionInstanceOrDefault(poseId);
		auto pose = CreateRunwayMotionInstanceOrDefault(poseId);
		// if (!pose.IsPose) {
		//     Console.WriteLine(...);
		//     pose = null;
		// }
		static auto method_MstRunwayMotion_get_IsPose =
			il2cpp_symbols_logged::get_method(
				"PRISM.Definitions.dll", "PRISM.Definitions",
				"MstRunwayMotion", "get_IsPose", 0);
		auto isPose = method_MstRunwayMotion_get_IsPose
			->Invoke((Il2CppObject*)pose, {})
			->unbox_value<bool>();
		if (!isPose) {
			std::cout << "[OverrideRunwayStartData] Motion id '" << walkingId << "' isn't a pose. Keeping unchanged." << std::endl;
			pose = nullptr;
		}

		// if (position < motions.Length) {
		//     var item = motions[position];
		//     if (walking != null) { item.WalkingMotion = walking; }
		//     if (pose != null) { item.PoseMotion = pose; }
		//     if (walking != null || pose != null) Console.WriteLine(...);
		// }
		if (position < il2cpp_array_length(motions)) {
			static auto method_RunwayEventMotion_set_WalkingMotion = il2cpp_symbols_logged::get_method(
				"PRISM.Legacy.dll", "PRISM.RunwayEvent", "RunwayEventMotion", "set_WalkingMotion", 1);
			static auto method_RunwayEventMotion_set_PoseMotionn = il2cpp_symbols_logged::get_method(
				"PRISM.Legacy.dll", "PRISM.RunwayEvent", "RunwayEventMotion", "set_PoseMotion", 1);

			auto item = (Il2CppObject*)il2cpp_symbols::array_get_value(motions, position);
			if (walking)
				method_RunwayEventMotion_set_WalkingMotion->InvokeAsVoid(item, { walking });
			if (pose)
				method_RunwayEventMotion_set_PoseMotionn->InvokeAsVoid(item, { pose });
			if (walking || pose) {
				std::cout << "[OverrideRunwayStartData] set position[" << position <<
					"] to walk '" << walkingId
					<< "', pose '" << poseId
					<< "'." << std::endl;
			}
		}
	}

	HOOK_ORIG_TYPE LiveStartDataExtensions_PreLoadAsync_orig;
	void* LiveStartDataExtensions_PreLoadAsync_hook(void* retstr, Il2CppObject* data, const MethodInfo* method) {
		RunwayDump::ForceSceneName(data);   // [TEST] v22 - env-driven scene override
		RunwayDump::DumpStartData(data);   // [DIAG] v21 - runway start data field dump
		try {   // [DIAG] v23 - arm the runway scene scan for runway runs
			auto cls = RunwayDump::ClassOf(data);
			const char* cn = cls ? il2cpp_class_get_name(cls) : nullptr;
			if (cn && strstr(cn, "Runway")) RunwaySceneScan::ArmSceneScan();
		}
		catch (...) {}
		auto onStageIdols = GetLiveStartDataOnStageIdols(data);
		if (onStageIdols != nullptr) {
			ModifyOnStageIdols(onStageIdols);
		}
		try { SoloUnlock::RemapAtPreLoad(data, onStageIdols); } catch (...) {}
		if (g_override_runway_motion) {
			for (int i = 0; i < overrideRunwayMotionData.size(); ++i) {
				OverrideRunwayStartData(data, i, overrideRunwayMotionData[i].WalkingMotionId, overrideRunwayMotionData[i].PoseMotionId);
			}
		}
		return HOOK_CAST_CALL(void*, LiveStartDataExtensions_PreLoadAsync)(retstr, data, method);
	}


	int getCharacterParamAliveCount(const std::string& objName) {
		int aliveCount = 0;
		for (auto& i : charaParam) {
			if (i.first.starts_with(objName)) {
				if (i.second.checkObjAlive()) {
					aliveCount++;
				}
			}
		}
		return aliveCount;
	}

	void AssembleCharacter_ApplyParam_hook(void* mdl, float height, float bust, float head, float arm, float hand) {
		if (g_enable_chara_param_edit) {
			static auto get_ObjectName = reinterpret_cast<Il2CppString * (*)(void*)>(
				il2cpp_symbols::get_method_pointer(
					"UnityEngine.CoreModule.dll", "UnityEngine",
					"Object", "GetName", 0
				)
				);
			const auto objNameIlStr = get_ObjectName(mdl);
			const std::string objName = objNameIlStr ? utility::conversions::to_utf8string(std::wstring(objNameIlStr->start_char)) : std::format("Unnamed Obj {:p}", mdl);
			std::string showObjName;
			if (objName.starts_with("m_ALL_")) {
				const auto nextFlagPos = objName.find_first_of(L'_', 6);
				if (nextFlagPos > 6) {
					showObjName = objName.substr(0, nextFlagPos);
				}
				else {
					showObjName = objName;
				}

				const auto objAliveCount = getCharacterParamAliveCount(showObjName);
				if (objAliveCount > 0) {
					showObjName = std::format("{} [{}]", showObjName, objAliveCount);
				}
				/*
				if (auto it = charaParam.find(showObjName); it != charaParam.end()) {
					if (it->second.checkObjAlive()) {
						showObjName = std::format("{} ({:p})", showObjName, mdl);
					}
				}*/
			}
			else {
				showObjName = objName;
			}
			if (auto it = charaParam.find(showObjName); it != charaParam.end()) {
				it->second.SetObjPtr(mdl);
				it->second.UpdateParam(&height, &bust, &head, &arm, &hand);
				return it->second.Apply();
			}
			else {
				charaParam.emplace(showObjName, CharaParam_t(height, bust, head, arm, hand, mdl));
			}
		}
		return HOOK_CAST_CALL(void, AssembleCharacter_ApplyParam)(mdl, height, bust, head, arm, hand);
	}

	HOOK_ORIG_TYPE MainThreadDispatcher_LateUpdate_orig;
	static bool g_saveAuditScanComplete = false;

	static std::string ToLowerAscii(std::string value) {
		for (char& c : value) {
			if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
		}
		return value;
	}

	static bool IsSaveAuditType(const char* namespaze, const char* className) {
		std::string typeName;
		if (namespaze) typeName += namespaze;
		typeName += "::";
		if (className) typeName += className;
		typeName = ToLowerAscii(std::move(typeName));
		return typeName.find("sqlite") != std::string::npos
			|| typeName.find("database") != std::string::npos
			|| typeName.find("savedata") != std::string::npos
			|| typeName.find("userdata") != std::string::npos
			|| typeName.find("localdata") != std::string::npos
			|| typeName.find("masterdata") != std::string::npos
			|| typeName.find("emberbox") != std::string::npos;
	}

	static void DumpSaveAuditMethodSignatures() {
		std::ofstream output("save-audit-methods.txt", std::ios::trunc);
		if (!output.is_open()) {
			printf("[save-audit] Cannot create save-audit-methods.txt\n");
			return;
		}

		output << "SCSP save-path metadata scan\n"
			<< "Method/type signatures only; no arguments, object values, or save files are read.\n\n";

		size_t assemblyCount = 0;
		auto** assemblies = il2cpp_domain_get_assemblies(il2cpp_domain_get(), &assemblyCount);
		size_t matchedTypes = 0;
		size_t writtenMethods = 0;
		constexpr size_t maxTypes = 500;
		constexpr size_t maxMethods = 5000;

		for (size_t a = 0; assemblies && a < assemblyCount && matchedTypes < maxTypes && writtenMethods < maxMethods; ++a) {
			auto* image = il2cpp_assembly_get_image((void*)assemblies[a]);
			if (!image) continue;
			const size_t classCount = il2cpp_image_get_class_count(image);
			for (size_t i = 0; i < classCount && matchedTypes < maxTypes && writtenMethods < maxMethods; ++i) {
				auto* klass = il2cpp_image_get_class(image, i);
				if (!klass) continue;
				const char* namespaze = il2cpp_class_get_namespace((void*)klass);
				const char* className = il2cpp_class_get_name((void*)klass);
				if (!IsSaveAuditType(namespaze, className)) continue;

				++matchedTypes;
				output << "TYPE " << (namespaze ? namespaze : "") << "::"
					<< (className ? className : "?") << "\n";
				void* iter = nullptr;
				const MethodInfo* method = nullptr;
				while ((method = il2cpp_class_get_methods((void*)klass, &iter)) && writtenMethods < maxMethods) {
					output << "  " << debug::FormatMethodInfo(method) << "\n";
					++writtenMethods;
				}
				output << "\n";
			}
		}

		output << "SUMMARY assemblies=" << assemblyCount
			<< " matchedTypes=" << matchedTypes
			<< " methods=" << writtenMethods << "\n";
		output.close();
		printf("[save-audit] Wrote %zu type(s), %zu method signature(s) to save-audit-methods.txt\n",
			matchedTypes, writtenMethods);
	}

	// ============================================================================
	// [DIAG] v15 - PRISM view-connector / viewMapping diagnostics (read-only).
	// ErrorCode[8E0AC0DA] == CRC32("IRunwayUnitConfirmationConnector"): produced by
	// BootMenuScene._toErrorCode() when a view jump cannot be resolved. These hooks
	// only log state (viewMapping content, jump targets, connector lifetime,
	// exceptions); they never modify game data.
	// ============================================================================
	namespace ViewDiag {
		void* g_bootMenuScene = nullptr;

		std::string timestamp() {
			const auto ms = GetTickCount64();
			return "[" + std::to_string((unsigned long long)(ms / 1000)) + "." + std::to_string((unsigned long long)(ms % 1000)) + "]";
		}

		std::string ptr_str(const void* p) {
			return std::format("{:p}", (void*)p);
		}

		std::string il2cpp_to_utf8(Il2CppString* s) {
			if (!s) return "(null)";
			try {
				return utility::conversions::to_utf8string(s->toWstring());
			}
			catch (...) {
				return "(convert-error)";
			}
		}

		void appendLog(const char* fileName, const std::string& line) {
			try {
				std::ofstream f(fileName, std::ios::app);
				if (f.is_open()) f << line << "\n";
			}
			catch (...) {}
		}

		void* find_class_anywhere(const char* namespaze, const char* className) {
			size_t count = 0;
			auto** assemblies = il2cpp_domain_get_assemblies(il2cpp_domain_get(), &count);
			for (size_t i = 0; assemblies && i < count; ++i) {
				auto* image = il2cpp_assembly_get_image((void*)assemblies[i]);
				if (!image) continue;
				auto* klass = il2cpp_class_from_name(image, namespaze, className);
				if (klass) return klass;
			}
			return nullptr;
		}

		void* find_method_anywhere(const char* namespaze, const char* className, const char* methodName, int paramCount) {
			auto* klass = find_class_anywhere(namespaze, className);
			if (!klass) return nullptr;
			auto* method = il2cpp_class_get_method_from_name(klass, methodName, paramCount);
			return method ? (void*)method->methodPointer : nullptr;
		}

		void* find_nested_move_next(const char* namespaze, const char* outerClassName, const char* nestedClassName) {
			auto* outer = (Il2CppClass*)find_class_anywhere(namespaze, outerClassName);
			if (!outer) return nullptr;
			auto* nested = il2cpp_symbols::get_nested_class(outer, nestedClassName);
			if (!nested) return nullptr;
			auto* method = il2cpp_class_get_method_from_name(nested, "MoveNext", 0);
			return method ? (void*)method->methodPointer : nullptr;
		}

		std::string reflection_type_name(Il2CppReflectionType* type) {
			if (!type) return "(null-type)";
			try {
				auto* klass = il2cpp_class_from_system_type(type);
				if (!klass) return "(unresolved-type)";
				const char* ns = il2cpp_class_get_namespace(klass);
				const char* name = il2cpp_class_get_name(klass);
				std::string result;
				if (ns && *ns) { result += ns; result += "."; }
				result += (name ? name : "?");
				return result;
			}
			catch (...) {
				return "(type-error)";
			}
		}

		void DumpViewMapping(const char* reason) {
			const std::string head = timestamp() + " dump(" + std::string(reason) + ")";
			try {
				if (!g_bootMenuScene) {
					appendLog("viewmap-dump.txt", head + " SKIP: BootMenuScene instance not captured yet");
					return;
				}
				auto* scene_klass = find_class_anywhere("PRISM.Boot", "BootMenuScene");
				if (!scene_klass) { appendLog("viewmap-dump.txt", head + " SKIP: BootMenuScene class not found"); return; }
				auto* mapping_field = il2cpp_class_get_field_from_name(scene_klass, "viewMapping");
				if (!mapping_field) { appendLog("viewmap-dump.txt", head + " SKIP: viewMapping field not found"); return; }
				auto* dict = il2cpp_symbols::read_field<Il2CppObject*>(g_bootMenuScene, mapping_field);
				if (!dict) { appendLog("viewmap-dump.txt", head + " viewMapping=NULL"); return; }

				auto* dict_klass = il2cpp_symbols::get_system_class_from_reflection_type_str(
					"System.Collections.Generic.Dictionary`2[System.String, PRISM.ViewBundle]");
				if (!dict_klass) { appendLog("viewmap-dump.txt", head + " SKIP: Dictionary<string,ViewBundle> type unresolved"); return; }

				auto* entries_field = il2cpp_class_get_field_from_name(dict_klass, "_entries");
				auto* count_field = il2cpp_class_get_field_from_name(dict_klass, "_count");
				auto* entry_klass = (void*)il2cpp_symbols::get_nested_class((Il2CppClass*)dict_klass, "Entry");
				auto* key_field = entry_klass ? il2cpp_class_get_field_from_name(entry_klass, "key") : nullptr;
				auto* value_field = entry_klass ? il2cpp_class_get_field_from_name(entry_klass, "value") : nullptr;
				auto* bundle_klass = find_class_anywhere("PRISM", "ViewBundle");
				auto* b_typeName_field = bundle_klass ? il2cpp_class_get_field_from_name(bundle_klass, "typeName") : nullptr;
				auto* b_reference_field = bundle_klass ? il2cpp_class_get_field_from_name(bundle_klass, "reference") : nullptr;
				auto* b_tags_field = bundle_klass ? il2cpp_class_get_field_from_name(bundle_klass, "tags") : nullptr;

				if (!entries_field || !count_field) {
					appendLog("viewmap-dump.txt", head + " SKIP: dictionary internals unresolved");
					return;
				}

				// Entry<string,ViewBundle> layout: hashCode@0, next@4, key@8, value@0x10.
				int key_off = 0x8;
				int value_off = 0x10;
				if (key_field && value_field) {
					key_off = key_field->offset;
					value_off = value_field->offset;
				}

				auto* entries = il2cpp_symbols::read_field<Il2CppObject*>(dict, entries_field);
				const int count = il2cpp_symbols::read_field<int>(dict, count_field);
				const int stride = value_off + (int)sizeof(void*);

				std::ofstream f("viewmap-dump.txt", std::ios::app);
				if (!f.is_open()) return;
				f << head << " COUNT=" << count << "\n";
				if (entries) {
					for (int i = 0; i < count; ++i) {
						const char* entry = il2cpp_array_addr_with_size(entries, stride, i);
						auto* key = *(Il2CppString**)(entry + key_off);
						auto* bundle = *(void**)(entry + value_off);
						f << "  [" << i << "] " << il2cpp_to_utf8(key);
						if (bundle && b_typeName_field) {
							auto* typeName = il2cpp_symbols::read_field<Il2CppString*>(bundle, b_typeName_field);
							auto* reference = b_reference_field ? il2cpp_symbols::read_field<Il2CppString*>(bundle, b_reference_field) : nullptr;
							f << "  typeName=" << il2cpp_to_utf8(typeName) << "  ref=" << il2cpp_to_utf8(reference);
							if (b_tags_field) {
								auto* tags = il2cpp_symbols::read_field<Il2CppObject*>(bundle, b_tags_field);
								if (tags) {
									const int tagCount = *(int*)((char*)tags + 0x18);
									f << "  tags=[";
									for (int t = 0; t < tagCount && t < 16; ++t) {
										auto* tag = *(Il2CppString**)il2cpp_array_addr_with_size(tags, (int)sizeof(void*), t);
										if (t) f << ",";
										f << il2cpp_to_utf8(tag);
									}
									f << "]";
								}
							}
						}
						else if (bundle) {
							f << "  (ViewBundle fields unresolved, obj=" << ptr_str(bundle) << ")";
						}
						f << "\n";
					}
				}
				f.close();
			}
			catch (...) {
				appendLog("viewmap-dump.txt", head + " EXCEPTION during dump");
			}
		}

		HOOK_ORIG_TYPE BootMenuScene_Start_MoveNext_orig;
		void BootMenuScene_Start_MoveNext_hook(void* _this) {
			try {
				if (_this && !g_bootMenuScene) {
					auto* scene = *(void**)((char*)_this + 0x10);
					if (scene) {
						g_bootMenuScene = scene;
						appendLog("viewmap-dump.txt", timestamp() + " BootMenuScene instance captured from <Start>d__0.MoveNext obj=" + ptr_str(scene));
					}
				}
			}
			catch (...) {}
			return HOOK_CAST_CALL(void, BootMenuScene_Start_MoveNext)(_this);
		}

		HOOK_ORIG_TYPE BootMenuScene_SelectView_MoveNext_orig;
		void BootMenuScene_SelectView_MoveNext_hook(void* _this) {
			try {
				if (_this) {
					const int state = *(int*)((char*)_this + 0x0);
					auto* scene = *(void**)((char*)_this + 0x20);
					if (scene && !g_bootMenuScene) g_bootMenuScene = scene;
					if (state == 0) {
						auto* viewType = *(Il2CppReflectionType**)((char*)_this + 0x28);
						appendLog("view-select-log.txt", timestamp() + " _selectViewAsync viewType=" + reflection_type_name(viewType));
					}
				}
			}
			catch (...) {}
			return HOOK_CAST_CALL(void, BootMenuScene_SelectView_MoveNext)(_this);
		}

		HOOK_ORIG_TYPE BootMenuScene_setupView_orig;
		int BootMenuScene_setupView_hook(void* _this) {
			try {
				if (_this && !g_bootMenuScene) {
					g_bootMenuScene = _this;
					appendLog("viewmap-dump.txt", timestamp() + " BootMenuScene instance captured from _setupView obj=" + ptr_str(_this));
				}
			}
			catch (...) {}
			return HOOK_CAST_CALL(int, BootMenuScene_setupView)(_this);
		}

		HOOK_ORIG_TYPE BootMenuScene_getViewBundles_orig;
		void* BootMenuScene_getViewBundles_hook(void* _this) {
			try {
				if (_this && !g_bootMenuScene) g_bootMenuScene = _this;
				appendLog("viewmap-dump.txt", timestamp() + " _getViewBundles called");
			}
			catch (...) {}
			return HOOK_CAST_CALL(void*, BootMenuScene_getViewBundles)(_this);
		}

		HOOK_ORIG_TYPE BootMenuScene_toErrorCode_orig;
		Il2CppString* BootMenuScene_toErrorCode_hook(Il2CppString* viewType) {
			try {
				appendLog("view-error-log.txt", timestamp() + " _toErrorCode viewType=" + il2cpp_to_utf8(viewType));
				DumpViewMapping("toErrorCode");
			}
			catch (...) {}
			return HOOK_CAST_CALL(Il2CppString*, BootMenuScene_toErrorCode)(viewType);
		}

		HOOK_ORIG_TYPE BootMenuScene_onViewErrorAsync_orig;
		void BootMenuScene_onViewErrorAsync_hook(void* retBuf, void* exception) {
			try {
				std::string detail = "(null)";
				if (exception) {
					auto* klass = il2cpp_symbols::get_class_from_instance(exception);
					if (klass) {
						const char* ns = il2cpp_class_get_namespace(klass);
						const char* name = il2cpp_class_get_name(klass);
						detail = std::string(ns ? ns : "") + "." + (name ? name : "?");
						auto* toString = il2cpp_class_get_method_from_name(klass, "ToString", 0);
						if (toString) {
							auto fn = reinterpret_cast<Il2CppString* (*)(void*)>(toString->methodPointer);
							detail += " msg=" + il2cpp_to_utf8(fn(exception));
						}
					}
				}
				appendLog("view-error-log.txt", timestamp() + " _onViewErrorAsync exception=" + detail);
				DumpViewMapping("onViewError");
			}
			catch (...) {}
			return HOOK_CAST_CALL(void, BootMenuScene_onViewErrorAsync)(retBuf, exception);
		}

		HOOK_ORIG_TYPE BootMenuScene_LoadViewAssetAsync_orig;
		void BootMenuScene_LoadViewAssetAsync_hook(void* retBuf, void* _this, Il2CppString* viewName, void* resourceLoader) {
			try {
				appendLog("view-asset-log.txt", timestamp() + " LoadViewAssetAsync viewName=" + il2cpp_to_utf8(viewName));
			}
			catch (...) {}
			return HOOK_CAST_CALL(void, BootMenuScene_LoadViewAssetAsync)(retBuf, _this, viewName, resourceLoader);
		}

		HOOK_ORIG_TYPE ViewManager_JumpView_orig;
		void ViewManager_JumpView_hook(void* _this, Il2CppReflectionType* viewType, bool withFade, void* param, int stackOperation) {
			try {
				appendLog("view-jump-log.txt", timestamp() + " JumpView viewType=" + reflection_type_name(viewType)
					+ " withFade=" + (withFade ? "1" : "0") + " stackOperation=" + std::to_string(stackOperation));
			}
			catch (...) {}
			return HOOK_CAST_CALL(void, ViewManager_JumpView)(_this, viewType, withFade, param, stackOperation);
		}

		HOOK_ORIG_TYPE ViewManager_JumpViewAsync_orig;
		void ViewManager_JumpViewAsync_hook(void* retBuf, void* _this, Il2CppReflectionType* viewType, bool withFade, void* param, int stackOperation) {
			try {
				appendLog("view-jump-log.txt", timestamp() + " JumpViewAsync viewType=" + reflection_type_name(viewType)
					+ " withFade=" + (withFade ? "1" : "0") + " stackOperation=" + std::to_string(stackOperation));
			}
			catch (...) {}
			return HOOK_CAST_CALL(void, ViewManager_JumpViewAsync)(retBuf, _this, viewType, withFade, param, stackOperation);
		}

		HOOK_ORIG_TYPE RunwayEventTopConnector_ctor_orig;
		void RunwayEventTopConnector_ctor_hook(void* _this) {
			try { appendLog("runway-connector-log.txt", timestamp() + " RunwayEventTopConnector..ctor obj=" + ptr_str(_this)); } catch (...) {}
			return HOOK_CAST_CALL(void, RunwayEventTopConnector_ctor)(_this);
		}

		HOOK_ORIG_TYPE RunwayEventTopConnector_ParseFromLegacy_orig;
		void* RunwayEventTopConnector_ParseFromLegacy_hook(void* _this, void* parameter) {
			try { appendLog("runway-connector-log.txt", timestamp() + " RunwayEventTopConnector.ParseFromLegacy param=" + ptr_str(parameter)); } catch (...) {}
			return HOOK_CAST_CALL(void*, RunwayEventTopConnector_ParseFromLegacy)(_this, parameter);
		}

		HOOK_ORIG_TYPE RunwayUnitConfirmationConnector_ctor_orig;
		void RunwayUnitConfirmationConnector_ctor_hook(void* _this) {
			try { appendLog("runway-connector-log.txt", timestamp() + " RunwayUnitConfirmationConnector..ctor obj=" + ptr_str(_this)); } catch (...) {}
			return HOOK_CAST_CALL(void, RunwayUnitConfirmationConnector_ctor)(_this);
		}

		HOOK_ORIG_TYPE RunwayUnitConfirmationConnector_ParseFromLegacy_orig;
		void* RunwayUnitConfirmationConnector_ParseFromLegacy_hook(void* _this, void* parameter) {
			try { appendLog("runway-connector-log.txt", timestamp() + " RunwayUnitConfirmationConnector.ParseFromLegacy param=" + ptr_str(parameter)); } catch (...) {}
			return HOOK_CAST_CALL(void*, RunwayUnitConfirmationConnector_ParseFromLegacy)(_this, parameter);
		}

		void Tick() {
			static ULONGLONG startTick = 0;
			static int stage = 0;
			if (!startTick) startTick = GetTickCount64();
			static const ULONGLONG thresholds[] = { 10000, 30000, 90000 };
			if (stage < 3 && GetTickCount64() - startTick >= thresholds[stage]) {
				DumpViewMapping(stage == 0 ? "T+10s" : (stage == 1 ? "T+30s" : "T+90s"));
				++stage;
			}
		}

		void InstallHooks() {
			auto BootMenuScene_Start_MoveNext_addr = find_nested_move_next("PRISM.Boot", "BootMenuScene", "<Start>d__0");
			auto BootMenuScene_SelectView_MoveNext_addr = find_nested_move_next("PRISM.Boot", "BootMenuScene", "<_selectViewAsync>d__5");
			auto BootMenuScene_setupView_addr = find_method_anywhere("PRISM.Boot", "BootMenuScene", "_setupView", 0);
			auto BootMenuScene_getViewBundles_addr = find_method_anywhere("PRISM.Boot", "BootMenuScene", "_getViewBundles", 0);
			auto BootMenuScene_toErrorCode_addr = find_method_anywhere("PRISM.Boot", "BootMenuScene", "_toErrorCode", 1);
			auto BootMenuScene_onViewErrorAsync_addr = find_method_anywhere("PRISM.Boot", "BootMenuScene", "_onViewErrorAsync", 1);
			auto BootMenuScene_LoadViewAssetAsync_addr = find_method_anywhere("PRISM.Boot", "BootMenuScene", "LoadViewAssetAsync", 2);
			auto ViewManager_JumpView_addr = find_method_anywhere("ENTERPRISE", "ViewManager", "JumpView", 4);
			auto ViewManager_JumpViewAsync_addr = find_method_anywhere("ENTERPRISE", "ViewManager", "JumpViewAsync", 4);
			auto RunwayEventTopConnector_ctor_addr = find_method_anywhere("PRISM.Interactions", "RunwayEventTopConnector", ".ctor", 0);
			auto RunwayEventTopConnector_ParseFromLegacy_addr = find_method_anywhere("PRISM.Interactions", "RunwayEventTopConnector", "ParseFromLegacy", 1);
			auto RunwayUnitConfirmationConnector_ctor_addr = find_method_anywhere("PRISM.Interactions", "RunwayUnitConfirmationConnector", ".ctor", 0);
			auto RunwayUnitConfirmationConnector_ParseFromLegacy_addr = find_method_anywhere("PRISM.Interactions", "RunwayUnitConfirmationConnector", "ParseFromLegacy", 1);

			ADD_HOOK_1(BootMenuScene_Start_MoveNext);
			ADD_HOOK_1(BootMenuScene_SelectView_MoveNext);
			ADD_HOOK_1(BootMenuScene_setupView);
			ADD_HOOK_1(BootMenuScene_getViewBundles);
			ADD_HOOK_1(BootMenuScene_toErrorCode);
			ADD_HOOK_1(BootMenuScene_onViewErrorAsync);
			ADD_HOOK_1(BootMenuScene_LoadViewAssetAsync);
			ADD_HOOK_1(ViewManager_JumpView);
			ADD_HOOK_1(ViewManager_JumpViewAsync);
			ADD_HOOK_1(RunwayEventTopConnector_ctor);
			ADD_HOOK_1(RunwayEventTopConnector_ParseFromLegacy);
			ADD_HOOK_1(RunwayUnitConfirmationConnector_ctor);
			ADD_HOOK_1(RunwayUnitConfirmationConnector_ParseFromLegacy);
		}
	}
	// =====================================================================
	// [PATCH] v24 - unlock EVERY song's "vocal separated" (分唱 / solo) parts.
	//
	//   Songs that support the part-splitting feature carry that state in the
	//   master (MstSong + the MasterData.SongPartsCharacters table).  A few
	//   songs only expose *some* of their characters as selectable solo
	//   vocals, because the rest are gated behind the "limited song parts"
	//   unlock (api.LiveService/UnlockLimitedSongParts -> HavingGroup, i.e. a
	//   product an offline profile can never own).  The client evaluates that
	//   gate in these two methods:
	//
	//     PRISM.Definitions.MstSong::CheckLimitedVocalSeparatedSatisfy() -> bool
	//     PRISM.Live.MusicData::IsLimitedVocalSeparatedLocked(<arg>)      -> bool
	//
	//   v24 replaces ONLY the two return values - no field writes, no master
	//   mutation, exactly like the v11 event-date fix:
	//       CheckLimitedVocalSeparatedSatisfy -> true
	//       IsLimitedVocalSeparatedLocked     -> false
	//
	//   Both detours ignore their arguments and never call the original, so a
	//   wrong parameter guess cannot corrupt the call: they only decide the
	//   bool that goes back to the caller.  SCSP_SOLO_UNLOCK=0 installs
	//   nothing at all.
	//
	//   SCSP_SOLO_ALL=1 additionally forces
	//     PRISM.Definitions.MstSong::IsCandidateAllCharacterVocalSeparated()
	//   -> true (installed only when that env var is set).
	//
	//   Diagnostics (append, game root):
	//     solo-unlock.txt        - install result, every forced call (first 40
	//                              each) and the class field dump of MstSong /
	//                              MstSongPartsCharacter / MusicData, so the
	//                              real master field names of the feature end
	//                              up on record.
	//     runway-master-dump.txt - the MasterData.SongPartsCharacters rows and
	//                              the full field dump of every MstSong row
	//                              that asked "am I locked?".
	// =====================================================================
	namespace SoloUnlock {
		bool g_enabled = true;      // SCSP_SOLO_UNLOCK=0 -> install nothing
		// [v27] SCSP_SOLO_ALL_SONGS=1 also opens IsSongParts on songs that do
		// not have the part feature at all.  Default OFF (v28): only songs that
		// already report IsSongParts=true get solo, exactly like v26.
		bool g_allSongs = false;
		bool g_forceAll = false;    // SCSP_SOLO_ALL=1    -> also force the
		                            //   "all characters are candidates" check
		bool g_installed = false;
		bool g_classDumped = false;
		bool g_tablesDumped = false;
		bool g_songsPatched = false;
		int g_satisfyCalls = 0, g_satisfyMstCalls = 0, g_lockedCalls = 0, g_lockedArgCalls = 0;
		int g_allCalls = 0, g_songDumps = 0, g_soloFlagCalls = 0, g_validModeCalls = 0;
		int g_lastSongId = -1;      // [DIAG] v30 - correlate lip tables with the active song row
		// [DIAG] v32 - part list / HashSet order caches for the solo index map
		std::vector<int> g_songPartsIds;
		int g_songPartsSongId = -1;
		bool g_partMethodsDumped = false;
		std::vector<int> g_soloVoiceIds;
		void* g_soloVoiceSet = nullptr;
		int g_soloMapLogs = 0;
		void* g_lastStartData = nullptr;
		int g_lastOrigSoloIndex = -1;
		int g_lastSelectedCharaId = -1;
		int g_lastMvSongId = -1;

		void Log(const std::string& line) {
			ViewDiag::appendLog("solo-unlock.txt", ViewDiag::timestamp() + " " + line);
		}

		// writer with a plain function-pointer type, for the RunwayDump helpers
		void ToSoloLog(const std::string& line) { Log(line); }

		void* KlassOf(void* obj) {
			if (!obj || !RunwayDump::PlausiblePtr((uintptr_t)obj)) return nullptr;
			try { return il2cpp_symbols::get_class_from_instance(obj); }
			catch (...) { return nullptr; }
		}

		FieldInfo* FieldOf(void* klass, const char* name) {
			if (!klass || !name) return nullptr;
			try { return il2cpp_symbols_logged::il2cpp_class_get_field_from_name(klass, name); }
			catch (...) { return nullptr; }
		}

		std::string StrOf(void* obj, const char* fieldName) {
			auto fi = FieldOf(KlassOf(obj), fieldName);
			if (!fi) return "";
			try { return ViewDiag::il2cpp_to_utf8(il2cpp_symbols::read_field<Il2CppString*>(obj, fi)); }
			catch (...) { return ""; }
		}

		// ---------------------------------------------------------------
		// one-shot class field dump - reveals the exact master field names of
		// the vocal-separated feature (IsSongParts, LimitedVocalSeparated*...)
		// ---------------------------------------------------------------
		void DumpClassFieldsOnce() {
			if (g_classDumped) return;
			g_classDumped = true;
			Log("=== v24 class field dump (read-only) ===");
			RunwayDump::DumpClassFields(il2cpp_symbols_logged::get_class("PRISM.Definitions.dll", "PRISM.Definitions", "MstSong"),
				"MstSong", ToSoloLog);
			RunwayDump::DumpClassFields(il2cpp_symbols_logged::get_class("PRISM.Definitions.dll", "PRISM.Definitions", "MstSongPartsCharacter"),
				"MstSongPartsCharacter", ToSoloLog);
			RunwayDump::DumpClassFields(il2cpp_symbols_logged::get_class("PRISM.Legacy.dll", "PRISM.Live", "MusicData"),
				"MusicData", ToSoloLog);
			Log("=== end of class field dump ===");
		}

		void* TryMasterData() {
			static auto method_get_Instance = il2cpp_symbols_logged::get_method(
				"PRISM.Adapters.dll", "PRISM.Adapters", "IAdapterLocator", "get_Instance", 0);
			if (!method_get_Instance || !method_get_Instance->methodPointer) return nullptr;
			Il2CppObject* locator = nullptr;
			try { locator = method_get_Instance->Invoke<Il2CppObject*>(nullptr, {}); }
			catch (...) { return nullptr; }
			if (!locator) return nullptr;
			auto locatorKlass = KlassOf(locator);
			auto getMasterData = locatorKlass ? il2cpp_class_get_method_from_name(locatorKlass, "get_MasterData", 0) : nullptr;
			if (!getMasterData || !getMasterData->methodPointer) return nullptr;
			try { return getMasterData->Invoke<Il2CppObject*>(locator, {}); }
			catch (...) { return nullptr; }
		}

		// ---------------------------------------------------------------
		// [DIAG] v32 - solo index mapping helpers
		// ---------------------------------------------------------------
		int FindIndex(const std::vector<int>& list, int value) {
			for (size_t i = 0; i < list.size(); ++i)
				if (list[i] == value) return (int)i;
			return -1;
		}

		std::string JoinIds(const std::vector<int>& list) {
			std::string out;
			for (size_t i = 0; i < list.size() && i < 32; ++i) {
				if (!out.empty()) out += ",";
				out += std::to_string(list[i]);
			}
			return out;
		}

		int ReadSongIdFromMvStage(void* mvStage) {
			if (!mvStage) return -1;
			auto k = KlassOf(mvStage);
			auto fi = FieldOf(k, "<Id>k__BackingField");
			if (!fi) fi = FieldOf(k, "Id");
			if (!fi) return -1;
			try { return il2cpp_symbols::read_field<int>(mvStage, fi); }
			catch (...) { return -1; }
		}

		std::string ReadMvSceneFromMvStage(void* mvStage) {
			if (!mvStage) return "";
			auto fi = FieldOf(KlassOf(mvStage), "<MvScene>k__BackingField");
			if (!fi) return "";
			try {
				return ViewDiag::il2cpp_to_utf8(
					il2cpp_symbols::read_field<Il2CppString*>(mvStage, fi));
			}
			catch (...) { return ""; }
		}

		// HashSet<int> iteration order without invoking the managed enumerator
		// (HashSet<T>.Enumerator is a struct, so a generic IEnumerable walk is
		// unsafe here).  Read _entries and try both plausible Entry<int>
		// strides; keep the sequence whose values look like character ids.
		void ReadHashSetIntOrder(void* set, std::vector<int>& out) {
			out.clear();
			if (!set || !RunwayDump::PlausiblePtr((uintptr_t)set)) return;
			auto k = KlassOf(set);
			auto fe = FieldOf(k, "_entries");
			auto fc = FieldOf(k, "_count");
			if (!fe || !fc) return;
			void* entries = nullptr;
			int count = -1;
			try {
				entries = il2cpp_symbols::read_field<void*>(set, fe);
				count = il2cpp_symbols::read_field<int>(set, fc);
			}
			catch (...) { return; }
			const char* ecn = entries ? RunwayDump::ClassNameOf(entries) : nullptr;
			if (!entries || !ecn || strstr(ecn, "[]") == nullptr || count <= 0 || count > 1000)
				return;
			int cap = (int)il2cpp_array_length(entries);
			if (cap <= 0 || cap > 512) return;
			std::vector<int> a, b;
			for (int i = 0; i < cap; ++i) {
				uintptr_t e12 = (uintptr_t)entries + 0x20 + (size_t)i * 12;
				if (*(int*)e12 >= 0 && (int)a.size() < count) a.push_back(*(int*)(e12 + 8));
				// only read the 16-byte variant while it still stays inside the
				// array allocation even if the real Entry<int> stride is 12
				if (i + 1 < cap) {
					uintptr_t e16 = (uintptr_t)entries + 0x20 + (size_t)i * 16;
					if (*(int*)e16 >= 0 && (int)b.size() < count) b.push_back(*(int*)(e16 + 8));
				}
			}
			auto plausible = [](const std::vector<int>& v) {
				if (v.empty()) return false;
				for (int x : v)
					if (x < 0 || x > 1000) return false;
				return true;
			};
			if (plausible(a)) out = a;
			else if (plausible(b)) out = b;
			Log(std::format("[v32] voice-ids-raw count={} cap={} s12=[{}] s16=[{}]",
				count, cap, JoinIds(a), JoinIds(b)));
		}

		bool DecodePartRow(void* row, int songId, int source) {
			if (!row) return false;
			auto fList = FieldOf(KlassOf(row), "<MstCharacterIdList>k__BackingField");
			if (!fList) {
				Log(std::format("[v34] part-row song={} source={} list-field-missing", songId, source));
				return false;
			}
			void* list = nullptr;
			try { list = il2cpp_symbols::read_field<void*>(row, fList); }
			catch (...) { list = nullptr; }
			if (!list) {
				Log(std::format("[v34] part-row song={} source={} list=null", songId, source));
				return false;
			}
			auto fSize = FieldOf(KlassOf(list), "_size");
			auto fItems = FieldOf(KlassOf(list), "_items");
			int size = -1;
			void* items = nullptr;
			if (fSize) try { size = il2cpp_symbols::read_field<int>(list, fSize); } catch (...) { size = -1; }
			if (fItems) try { items = il2cpp_symbols::read_field<void*>(list, fItems); } catch (...) { items = nullptr; }
			const char* icn = items ? RunwayDump::ClassNameOf(items) : nullptr;
			if (size < 0 || size > 64 || !items || !icn || !strstr(icn, "[]")) {
				Log(std::format("[v34] part-row song={} source={} bad-list size={} items={}",
					songId, source, size, RunwayDump::PtrStr(items)));
				return false;
			}
			g_songPartsIds.clear();
			for (int j = 0; j < size; ++j)
				g_songPartsIds.push_back(*(int*)((uintptr_t)items + 0x20 + (size_t)j * 4));
			g_songPartsSongId = songId;
			Log(std::format("[v34] part-row song={} source={} size={} ids=[{}]",
				songId, source, size, JoinIds(g_songPartsIds)));
			return true;
		}

		void* LookupPartRow(void* table, int songId) {
			auto k = KlassOf(table);
			if (!k) return nullptr;
			const char* names[] = { "get_Item", "Get", "GetValue" };
			for (const char* name : names) {
				auto mi = il2cpp_class_get_method_from_name(k, name, 1);
				if (!mi || !mi->methodPointer) continue;
				try {
					auto row = mi->Invoke<Il2CppObject*>((Il2CppObject*)table, { (Il2CppObject*)&songId });
					Log(std::format("[v34] part-lookup {} song={} -> {}", name, songId, RunwayDump::PtrStr(row)));
					if (row) return row;
				}
				catch (...) {
					Log(std::format("[v34] part-lookup {} song={} threw", name, songId));
				}
			}
			if (!g_partMethodsDumped) {
				g_partMethodsDumped = true;
				void* iter = nullptr;
				int n = 0;
				while (auto m = il2cpp_class_get_methods(k, &iter)) {
					if (n++ > 80) break;
					Log(std::format("[v34] table-method {} argc={}",
						m->name ? m->name : "?", (int)m->parameters_count));
				}
			}
			return nullptr;
		}

		// One line per newly seen song: the MstSongPartsCharacter row and the
		// ordered MstCharacterIdList (the part order used by the song data).
		void DumpSongPartsForSong(int songId) {
			if (songId <= 0 || songId == g_songPartsSongId) return;
			static int triedSong = -1;
			static int tries = 0;
			if (songId != triedSong) { triedSong = songId; tries = 0; }
			if (tries++ >= 3) return;
			g_songPartsIds.clear();
			auto masterData = TryMasterData();
			if (!masterData) {
				Log(std::format("[v32] part-list song={} master=null", songId));
				return;
			}
			auto fi = FieldOf(KlassOf(masterData), "SongPartsCharacters");
			if (!fi) {
				Log(std::format("[v32] part-list song={} field-missing", songId));
				return;
			}
			void* table = nullptr;
			try { table = il2cpp_symbols::read_field<void*>(masterData, fi); }
			catch (...) { table = nullptr; }
			void* arr = nullptr;
			if (table) {
				auto fd = FieldOf(KlassOf(table), "data");
				if (fd) try { arr = il2cpp_symbols::read_field<void*>(table, fd); } catch (...) { arr = nullptr; }
			}
			const char* acn = arr ? RunwayDump::ClassNameOf(arr) : nullptr;
			const int len = (arr && acn && strstr(acn, "[]")) ? (int)il2cpp_array_length(arr) : -1;
			Log(std::format("[v32] part-list song={} table={} arr={} len={}",
				songId, RunwayDump::PtrStr(table), RunwayDump::PtrStr(arr), len));
			if (len <= 0 || len > 512) return;
			for (int i = 0; i < len; ++i) {
				void* row = nullptr;
				try { row = il2cpp_symbols::array_get_value(arr, i); }
				catch (...) { row = nullptr; }
				if (!row) continue;
				auto fId = FieldOf(KlassOf(row), "<MstSongId>k__BackingField");
				if (!fId) continue;
				int rowSong = -1;
				try { rowSong = il2cpp_symbols::read_field<int>(row, fId); }
				catch (...) { continue; }
				if (rowSong != songId) continue;
				if (DecodePartRow(row, songId, i)) return;
				return;
			}
			void* looked = LookupPartRow(table, songId);
			if (looked && DecodePartRow(looked, songId, -1)) return;
			Log(std::format("[v32] part-list song={} row-not-loaded", songId));
		}

		std::string ReadSoloMapMode() {
			char buf[64]{};
			FILE* f = fopen("solo-map.txt", "r");
			if (f) {
				if (!fgets(buf, sizeof(buf), f)) buf[0] = '\0';
				fclose(f);
			}
			std::string s = buf;
			auto trim = [](std::string& v) {
				while (!v.empty() && (v.back() == '\r' || v.back() == '\n' || v.back() == ' ' || v.back() == '\t'))
					v.pop_back();
				size_t p = v.find_first_not_of(" \t");
				if (p == std::string::npos) v.clear();
				else v = v.substr(p);
			};
			trim(s);
			auto eq = s.find('=');
			if (eq != std::string::npos) { s = s.substr(eq + 1); trim(s); }
			const char* env = getenv("SCSP_SOLO_MAP");
			if (env && *env) s = env;
			if (s.empty()) s = "post";
			return s;
		}

		void ApplySoloIndexMap(void* _this, void* onStageIdols, int origIndex, int charaId, int songId) {
			if (g_soloMapLogs++ > 60) return;
			const std::string mode = ReadSoloMapMode();
			auto k = KlassOf(_this);
			if (!k) return;
			auto fiArr = FieldOf(k, "<OnStageIdols>k__BackingField");
			if (!fiArr) fiArr = FieldOf(k, "OnStageIdols");
			void* postArr = nullptr;
			if (fiArr) try { postArr = il2cpp_symbols::read_field<void*>(_this, fiArr); } catch (...) { postArr = nullptr; }
			if (!postArr) postArr = onStageIdols;
			int postIdx = -1;
			std::vector<int> postIds;
			if (postArr) {
				int len = (int)il2cpp_array_length(postArr);
				for (int i = 0; i < len && i < 32; ++i) {
					auto idol = (managed::UnitIdol*)il2cpp_symbols::array_get_value(postArr, i);
					int id = idol ? UnitIdol::GetCharaId(idol) : -1;
					postIds.push_back(id);
					if (id == charaId && postIdx < 0) postIdx = i;
				}
			}
			const int partIdx = (g_songPartsSongId == songId) ? FindIndex(g_songPartsIds, charaId) : -1;
			const int hashIdx = FindIndex(g_soloVoiceIds, charaId);
			int newIndex = origIndex;
			if (mode == "post" && postIdx >= 0) newIndex = postIdx;
			else if (mode == "part" && partIdx >= 0) newIndex = partIdx;
			else if (mode == "hash" && hashIdx >= 0) newIndex = hashIdx;
			else if (mode == "prev") newIndex = origIndex > 0 ? origIndex - 1 : 0;
			else if (mode == "next" && origIndex + 1 < (int)postIds.size()) newIndex = origIndex + 1;
			auto fiIdx = FieldOf(k, "<VocalSeparatedSoloIndex>k__BackingField");
			if (!fiIdx) fiIdx = FieldOf(k, "VocalSeparatedSoloIndex");
			int fieldBefore = -999, fieldAfter = -999;
			if (fiIdx) try { fieldBefore = il2cpp_symbols::read_field<int>(_this, fiIdx); } catch (...) { fieldBefore = -999; }
			if (fiIdx && newIndex != origIndex && newIndex >= 0 &&
				(postIds.empty() || newIndex < (int)postIds.size())) {
				int value = newIndex;
				il2cpp_field_set_value(_this, fiIdx, &value);
			}
			if (fiIdx) try { fieldAfter = il2cpp_symbols::read_field<int>(_this, fiIdx); } catch (...) { fieldAfter = -999; }
			Log(std::format("[v32] solo-map mode={} song={} chara={} orig={} post={} part={} hash={} new={} field={}->{} post=[{}] hashIds=[{}]",
				mode, songId, charaId, origIndex, postIdx, partIdx, hashIdx, newIndex,
				fieldBefore, fieldAfter, JoinIds(postIds), JoinIds(g_soloVoiceIds)));
		}

		void RememberStartData(void* data, int origIndex, int charaId, int songId) {
			g_lastStartData = data;
			g_lastOrigSoloIndex = origIndex;
			g_lastSelectedCharaId = charaId;
			g_lastMvSongId = songId;
		}

		// [v32] second chance: by PreLoadAsync the SongParts row and the
		// post-sort lineup should both be available.  Recompute from the
		// original (pre-map) index captured in the ctor.
		void RemapAtPreLoad(void* data, void* onStageIdols) {
			if (!data || data != g_lastStartData) return;
			DumpSongPartsForSong(g_lastMvSongId);
			int idx = g_lastOrigSoloIndex;
			int chara = g_lastSelectedCharaId;
			if (chara < 0 && onStageIdols && idx >= 0) {
				int len = (int)il2cpp_array_length(onStageIdols);
				if (idx < len) {
					auto idol = (managed::UnitIdol*)il2cpp_symbols::array_get_value(onStageIdols, idx);
					if (idol) chara = UnitIdol::GetCharaId(idol);
				}
			}
			ApplySoloIndexMap(data, onStageIdols, idx, chara, g_lastMvSongId);
		}

		// ---------------------------------------------------------------
		// one-shot: every row of MasterData.SongPartsCharacters - the master
		// table that maps a song to the characters taking part in 分唱.
		// Rows are written through the v21 read-only table walker.
		// ---------------------------------------------------------------
		void DumpPartsCharactersOnce() {
			if (g_tablesDumped) return;
			auto masterData = TryMasterData();
			if (!masterData) return;
			g_tablesDumped = true;
			auto fi = FieldOf(KlassOf(masterData), "SongPartsCharacters");
			if (!fi) { Log("[v24] MasterData.SongPartsCharacters field not found"); return; }
			void* table = nullptr;
			try { table = il2cpp_symbols::read_field<void*>(masterData, fi); }
			catch (...) { table = nullptr; }
			Log("[v24] dumping MasterData.SongPartsCharacters rows into runway-master-dump.txt");
			try { RunwayDump::DumpTableRows(table, "MstSongPartsCharacter(v24)", 1000); }
			catch (...) { Log("[v24] SongPartsCharacters dump threw"); }
		}

		// ---------------------------------------------------------------
		// dump the full field set of the MstSong row that was asked about
		// ---------------------------------------------------------------
		bool ReadBoolF(void* obj, const char* fieldName) {
			auto fi = FieldOf(KlassOf(obj), fieldName);
			if (!fi) return false;
			try { return il2cpp_symbols::read_field<bool>(obj, fi); }
			catch (...) { return false; }
		}

		int ReadIntF(void* obj, const char* fieldName, int def = -1) {
			auto fi = FieldOf(KlassOf(obj), fieldName);
			if (!fi) return def;
			try { return il2cpp_symbols::read_field<int>(obj, fi); }
			catch (...) { return def; }
		}

		void DumpSongRow(void* musicData, int callNo) {
			if (g_songDumps >= 10) return;
			auto fi = FieldOf(KlassOf(musicData), "<Master>k__BackingField");
			if (!fi) {
				if (callNo == 1) Log("  (MusicData.<Master>k__BackingField not found)");
				return;
			}
			void* row = nullptr;
			try { row = il2cpp_symbols::read_field<void*>(musicData, fi); }
			catch (...) { return; }
			if (!row || !RunwayDump::PlausiblePtr((uintptr_t)row)) return;
			++g_songDumps;
			g_lastSongId = ReadIntF(row, "<Id>k__BackingField");
			std::string name = StrOf(row, "<Name>k__BackingField");
			if (name.empty()) name = StrOf(row, "<SongName>k__BackingField");
			if (name.empty()) name = StrOf(row, "<Title>k__BackingField");
			if (name.empty()) name = "(name field unknown)";
			Log(std::format("  song row #{} id={} name=\"{}\" isSongParts={} isSongPartsSolo={}",
				g_songDumps, g_lastSongId, name,
				ReadBoolF(row, "<IsSongParts>k__BackingField") ? 1 : 0,
				ReadBoolF(row, "<IsSongPartsSolo>k__BackingField") ? 1 : 0));
			auto unlockListField = FieldOf(KlassOf(row), "<LimitedSongPartsUnlockProductList>k__BackingField");
			if (unlockListField) {
				void* lst = nullptr;
				try { lst = il2cpp_symbols::read_field<void*>(row, unlockListField); }
				catch (...) { lst = nullptr; }
				const char* cn = (lst && RunwayDump::PlausiblePtr((uintptr_t)lst)) ? RunwayDump::ClassNameOf(lst) : nullptr;
				int cnt = -1;
				if (lst && RunwayDump::PlausiblePtr((uintptr_t)lst)) {
					auto lstKlass = il2cpp_symbols::get_class_from_instance(lst);
					auto sizeField = lstKlass ? il2cpp_class_get_field_from_name(lstKlass, "_size") : nullptr;
					if (sizeField) { try { cnt = il2cpp_symbols::read_field<int>(lst, sizeField); } catch (...) { cnt = -1; } }
				}
				Log(std::format("  LimitedSongPartsUnlockProductList = {} ({}) size={}",
					lst ? RunwayDump::PtrStr(lst) : "(null)", cn ? cn : "?", cnt));
			}
			else {
				Log("  (no <LimitedSongPartsUnlockProductList> field on this row)");
			}
			Log("  (full field dump follows in runway-master-dump.txt)");
			try { RunwayDump::ReflectDump(row, 1, "MstSong(v24)", false); }
			catch (...) { Log("  (ReflectDump threw)"); }
		}

		// ---------------------------------------------------------------
		// detours - constant returns, arguments are never touched.
		//
		// Name forms verified against global-metadata.dat:
		//   MusicData.get_IsLimitedVocalSeparatedLocked  (property, 0 args)
		//   MusicData.IsLimitedVocalSeparatedLocked      (1 arg, if present)
		//   MusicData.CheckLimitedVocalSeparatedSatisfy  (1 arg)
		//   MstSong.CheckLimitedVocalSeparatedSatisfy    (0 args, if present)
		//   MusicData.get_IsAllCharacterVocalSeparated   (property, 0 args)
		// None of them calls the original, so the ABI cannot be disturbed
		// even if an argument is typed differently than expected.
		// ---------------------------------------------------------------
		// [v26] field writer (defined below) - declared here for the detours
		bool PatchSongRow(void* row, const char* why);
		// [v28] song-status writer + solo-voice ownership probe
		void PatchSongStatus(void* musicData, const char* why);
		void LogOwnedSoloVoices(void* musicData, const char* why);

		HOOK_ORIG_TYPE SoloLocked_get_orig;
		bool SoloLocked_get_hook(void* _this, void* mtd) {
			// [v26] the MusicData being asked about == the song being looked
			// at, so write its master flag before the UI reads it again
			auto masterFi = FieldOf(KlassOf(_this), "<Master>k__BackingField");
			if (masterFi) {
				void* row = nullptr;
				try { row = il2cpp_symbols::read_field<void*>(_this, masterFi); }
				catch (...) { row = nullptr; }
				PatchSongRow(row, "get_IsLimitedVocalSeparatedLocked");
			}
			// [v28] pretend the "limited song parts" product was bought and
			// record which solo voices the client thinks we own
			PatchSongStatus(_this, "get_IsLimitedVocalSeparatedLocked");
			LogOwnedSoloVoices(_this, "get_IsLimitedVocalSeparatedLocked");
			const int n = ++g_lockedCalls;
			if (n <= 40) {
				Log(std::format("[v24] MusicData.get_IsLimitedVocalSeparatedLocked -> false (#{})", n));
				DumpSongRow(_this, n);
			}
			return false;
		}

		// [v26] trigger + passthrough: if the panel asks MusicData for the
		// valid vocal-separated modes, patch the master row first and then
		// let the original compute the answer from the patched field.
		HOOK_ORIG_TYPE MusicData_GetValidVocalSeparatedMode_orig;
		void* MusicData_GetValidVocalSeparatedMode_hook(void* _this, void* mtd) {
			auto masterFi = FieldOf(KlassOf(_this), "<Master>k__BackingField");
			if (masterFi) {
				void* row = nullptr;
				try { row = il2cpp_symbols::read_field<void*>(_this, masterFi); }
				catch (...) { row = nullptr; }
				PatchSongRow(row, "GetValidVocalSeparatedMode");
			}
			PatchSongStatus(_this, "GetValidVocalSeparatedMode");
			LogOwnedSoloVoices(_this, "GetValidVocalSeparatedMode");
			// pointer-sized return covers enum (eax), pointer and object alike
			void* ret = HOOK_CAST_CALL(void*, MusicData_GetValidVocalSeparatedMode)(_this, mtd);
			if (++g_validModeCalls <= 40) {
				std::string extra;
				if (ret && RunwayDump::PlausiblePtr((uintptr_t)ret)) {
					const char* cn = RunwayDump::ClassNameOf(ret);
					if (cn) extra = std::string(" class=") + cn;
				}
				Log(std::format("[v26] MusicData.GetValidVocalSeparatedMode -> {} (as int {}){}",
					ViewDiag::ptr_str(ret), (int)(intptr_t)ret, extra));
			}
			return ret;
		}

		HOOK_ORIG_TYPE SoloLocked_arg_orig;
		bool SoloLocked_arg_hook(void* _this, void* arg, void* mtd) {
			const int n = ++g_lockedArgCalls;
			if (n <= 40)
				Log(std::format("[v24] MusicData.IsLimitedVocalSeparatedLocked(arg={}) -> false (#{})",
					ViewDiag::ptr_str(arg), n));
			return false;
		}

		HOOK_ORIG_TYPE SoloSatisfy_music_orig;
		bool SoloSatisfy_music_hook(void* _this, void* arg, void* mtd) {
			if (++g_satisfyCalls <= 40)
				Log(std::format("[v24] MusicData.CheckLimitedVocalSeparatedSatisfy(arg={}) -> true (#{})",
					ViewDiag::ptr_str(arg), g_satisfyCalls));
			return true;
		}

		HOOK_ORIG_TYPE SoloSatisfy_mst_orig;
		bool SoloSatisfy_mst_hook(void* _this, void* mtd) {
			if (++g_satisfyMstCalls <= 40)
				Log(std::format("[v24] MstSong.CheckLimitedVocalSeparatedSatisfy -> true (#{})", g_satisfyMstCalls));
			return true;
		}

		HOOK_ORIG_TYPE SoloAllCharas_orig;
		bool SoloAllCharas_hook(void* _this, void* mtd) {
			if (++g_allCalls <= 40)
				Log(std::format("[v24] MusicData.get_IsAllCharacterVocalSeparated -> true (#{})", g_allCalls));
			return true;
		}

		// ---------------------------------------------------------------
		// [v25] the real gate for songs like "Dye the sky." (#37, 037_DyeSky):
		//   MstSong #37  IsSongParts = true,  IsSongPartsSolo = false
		// so the 歌唱設定 panel greys out ソロ with
		//   "この楽曲では対応していません�?
		// The master row is immutable, so only the *getter* is overridden:
		// songs that already report IsSongParts=true are told solo is
		// supported; songs without the part feature keep their value.
		// ---------------------------------------------------------------
		HOOK_ORIG_TYPE MstSong_get_IsSongPartsSolo_orig;
		bool MstSong_get_IsSongPartsSolo_hook(void* _this, void* mtd) {
			const bool parts = ReadBoolF(_this, "<IsSongParts>k__BackingField");
			const int n = ++g_soloFlagCalls;
			if (n <= 40)
				Log(std::format("[v25] MstSong#{} get_IsSongPartsSolo: isSongParts={} -> {}",
					ReadIntF(_this, "<Id>k__BackingField"), parts ? 1 : 0, parts ? "true" : "false(native)"));
			if (!parts) return false;
			return true;
		}

		// ---------------------------------------------------------------
		// [v26] one byte, one row: write <IsSongPartsSolo> = true on master
		// rows that already report <IsSongParts> = true.  The v25 run proved
		// the UI never calls the property getter (zero call lines), so the
		// flag has to be written where every reader looks: the field itself.
		// Same technique as the v14/v15 Shiny Runway EndDate patch.
		// ---------------------------------------------------------------
		int g_rowsPatched = 0;
		int g_patchLogs = 0;

		bool PatchSongRow(void* row, const char* why) {
			if (!row || !RunwayDump::PlausiblePtr((uintptr_t)row)) return false;
			auto klass = KlassOf(row);
			auto fiSolo = FieldOf(klass, "<IsSongPartsSolo>k__BackingField");
			auto fiParts = FieldOf(klass, "<IsSongParts>k__BackingField");
			if (!fiSolo || !fiParts) return false;                     // not an MstSong row
			if (fiSolo->offset < 0x10 || fiSolo->offset > 0x8000) return false;
			if (fiParts->offset < 0x10 || fiParts->offset > 0x8000) return false;
			const bool solo = ReadBoolF(row, "<IsSongPartsSolo>k__BackingField");
			const bool parts = ReadBoolF(row, "<IsSongParts>k__BackingField");
			const bool wantParts = g_allSongs ? true : parts;
			if (!wantParts && !solo) return false;
			int wrote = 0;
			if (wantParts && !parts) {
				*(reinterpret_cast<uint8_t*>((uintptr_t)row + (size_t)fiParts->offset)) = 1;
				++wrote;
			}
			if (!solo) {
				*(reinterpret_cast<uint8_t*>((uintptr_t)row + (size_t)fiSolo->offset)) = 1;
				++wrote;
			}
			if (!wrote) return false;
			++g_rowsPatched;
			if (g_patchLogs++ < 60)
				Log(std::format("[v27] patched MstSong#{} parts {}->1 solo {}->1 ({}) total={}",
					ReadIntF(row, "<Id>k__BackingField"), parts ? 1 : 0, solo ? 1 : 0, why, g_rowsPatched));
			return true;
		}

		// ---------------------------------------------------------------
		// [v28] a character's solo voice is a separate PRODUCT
		//   LimitedVoiceSeparate_{charId}.usm
		// bought through LimitedVoiceSeparatePurchaseOverlayViewModel
		// (fields Price / CharacterIds / SongName / MusicId).  The client
		// trusts ISongStatus.IsLimitedSongPartsUnlocked for "already owned";
		// force that one byte so the audio + lip-sync path (the one that knows
		// who sings) is taken instead of the "not acquired" path, which is
		// exactly what produced "nobody opens their mouth / wrong singer".
		// ---------------------------------------------------------------
		void PatchSongStatus(void* musicData, const char* why) {
			auto songFi = FieldOf(KlassOf(musicData), "<Song>k__BackingField");
			if (!songFi) return;
			void* st = nullptr;
			try { st = il2cpp_symbols::read_field<void*>(musicData, songFi); }
			catch (...) { st = nullptr; }
			if (!st || !RunwayDump::PlausiblePtr((uintptr_t)st)) return;
			auto klass = KlassOf(st);
			if (!klass) return;
			for (const char* nm : { "<IsLimitedSongPartsUnlocked>k__BackingField",
				"<isLimitedSongPartsUnlocked>k__BackingField",
				"isLimitedSongPartsUnlocked" }) {
				auto fi = FieldOf(klass, nm);
				if (!fi || fi->offset < 0x10 || fi->offset > 0x8000) continue;
				bool cur = false;
				try { cur = il2cpp_symbols::read_field<bool>(st, fi); }
				catch (...) { continue; }
				if (cur) return;                                     // already unlocked
				*(reinterpret_cast<uint8_t*>((uintptr_t)st + (size_t)fi->offset)) = 1;
				Log(std::format("[v28] patched SongStatus {} = true ({})", nm, why));
				return;
			}
		}

		// read-only probe: which solo voices does the client think we own?
		void LogOwnedSoloVoices(void* musicData, const char* why) {
			auto fi = FieldOf(KlassOf(musicData), "limitedVocalSeparatedCharacterIds");
			if (!fi) {
				Log(std::format("[v28] no limitedVocalSeparatedCharacterIds field ({})", why));
				return;
			}
			void* set = nullptr;
			try { set = il2cpp_symbols::read_field<void*>(musicData, fi); }
			catch (...) { set = nullptr; }
			if (!set || !RunwayDump::PlausiblePtr((uintptr_t)set)) {
				Log(std::format("[v28] limitedVocalSeparatedCharacterIds = null ({})", why));
				return;
			}
			int cnt = -1;
			auto cntFi = FieldOf(KlassOf(set), "_count");
			if (cntFi) {
				try { cnt = il2cpp_symbols::read_field<int>(set, cntFi); }
				catch (...) { cnt = -1; }
			}
			// [v32] capture the HashSet iteration order once per MusicData
			// instance.  HashSet<T>.Enumerator is a struct, so the generic
			// IEnumerable helper is not safe; ReadHashSetIntOrder walks the
			// _entries array instead.
			if (set != g_soloVoiceSet) {
				g_soloVoiceSet = set;
				ReadHashSetIntOrder(set, g_soloVoiceIds);
				Log(std::format("[v32] voice-ids stored count={} ids=[{}] ({})",
					cnt, JoinIds(g_soloVoiceIds), why));
			}
			const std::string ids = g_soloVoiceIds.empty() ? "(raw-empty)" : JoinIds(g_soloVoiceIds);
			const char* cn = RunwayDump::ClassNameOf(set);
			Log(std::format("[v32] limitedVocalSeparatedCharacterIds count={} class={} ids=[{}] ({})",
				cnt, cn ? cn : "?", ids, why));
		}

		// walk MasterData.Songs once and patch every part-capable song
		void PatchAllSongs() {
			auto masterData = TryMasterData();
			if (!masterData) return;
			auto fi = FieldOf(KlassOf(masterData), "Songs");
			if (!fi) { Log("[v26] MasterData.Songs not found"); return; }
			void* table = nullptr;
			try { table = il2cpp_symbols::read_field<void*>(masterData, fi); }
			catch (...) { table = nullptr; }
			if (!table) { Log("[v26] MasterData.Songs = null"); return; }
			void* arr = RunwayDump::ReadPtr(table, "data");
			int n = RunwayDump::ArrayLength(arr);
			if (n <= 0) {
				auto items = RunwayDump::ReadPtr(arr, "_items");
				int size = RunwayDump::ReadInt(arr, "_size");
				if (items && size > 0) { arr = items; n = size; }
			}
			if (n <= 0) return;
			g_songsPatched = true;
			Log(std::format("[v26] MasterData.Songs rows = {}", n));
			int parts = 0, solo = 0;
			for (int i = 0; i < n && i < 2000; ++i) {
				void* row = (void*)(*(uintptr_t*)RunwayDump::ArrayElem(arr, i, 8));
				if (!row) continue;
				if (ReadBoolF(row, "<IsSongParts>k__BackingField")) ++parts;
				if (ReadBoolF(row, "<IsSongPartsSolo>k__BackingField")) ++solo;
				PatchSongRow(row, "Songs table");
			}
			Log(std::format("[v27] songs: total={} parts(native)={} solo(native)={} patched={} allSongs={}",
				n, parts, solo, g_rowsPatched, g_allSongs ? 1 : 0));
		}


		void* MethodAddr(const char* assembly, const char* ns, const char* klass, const char* name, int argc) {
			auto k = il2cpp_symbols_logged::get_class(assembly, ns, klass);
			if (!k) { Log(std::format("[v24] class {}.{} not found in {}", ns, klass, assembly)); return nullptr; }
			auto mi = il2cpp_class_get_method_from_name(k, name, argc);
			if (!mi || !mi->methodPointer) {
				Log(std::format("[v24] {}.{}::{}({}) not found", ns, klass, name, argc));
				return nullptr;
			}
			std::string params;
			const uint32_t pc = il2cpp_method_get_param_count(mi);
			for (uint32_t i = 0; i < pc && i < 8; ++i) {
				if (i) params += ", ";
				const char* tn = il2cpp_symbols::il2cpp_method_get_param_type_name(mi, i);
				params += tn ? tn : "?";
			}
			auto rt = il2cpp_method_get_return_type(mi);
			const char* rtn = rt ? il2cpp_type_get_name(rt) : "?";
			Log(std::format("[v24] {}.{}::{}({}) : {} @ {}", ns, klass, name, params,
				rtn ? rtn : "?", ViewDiag::ptr_str((void*)mi->methodPointer)));
			return (void*)mi->methodPointer;
		}

		bool HookAt(const char* label, void* target, void* detour, HOOK_ORIG_TYPE& orig) {
			if (!target) return false;
#ifdef __SAFETYHOOK
			AddSafetyHook(label, target, detour, orig);
#else
			MH_CreateHook(target, detour, &orig);
			MH_EnableHook(target);
#endif
			return true;
		}


		// ---------------------------------------------------------------
		// [v33] minimal, file-gated lip fallback.
		//
		//   `lip.txt` (game root):
		//     diag   - install the detour, log only (no table dereference)
		//     on     - also cache the last non-solo lip table for the current
		//              song and reuse it when solo setup receives null
		//     off / missing - do not install at all
		//
		//   The v30 crash likely came from the reflection dumps on an
		//   unverified table pointer.  This version never calls ClassOf /
		//   ReflectDump / DumpClassFields from the detour.
		// ---------------------------------------------------------------
		std::string ReadLipMode() {
			char buf[32]{};
			FILE* f = fopen("lip.txt", "r");
			if (f) {
				if (!fgets(buf, sizeof(buf), f)) buf[0] = '\0';
				fclose(f);
			}
			std::string s = buf;
			while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t'))
				s.pop_back();
			size_t p = s.find_first_not_of(" \t");
			if (p == std::string::npos) s.clear(); else s = s.substr(p);
			return s;
		}

		void* g_lipCachedHandle = nullptr;
		int g_lipCachedSong = -1;
		int g_lipHookCalls = 0;
		int g_lipHookSubs = 0;
		int g_lipFlagLogs = 0;
		bool g_lipForceMode = false;
		std::mutex g_lipMutex;

		bool LipReadBool(void* obj, const char* name) {
			if (!obj) return false;
			auto fi = FieldOf(KlassOf(obj), name);
			if (!fi) return false;
			try { return il2cpp_symbols::read_field<bool>(obj, fi); }
			catch (...) { return false; }
		}

		int LipCharaId(void* ch) {
			if (!ch) return -1;
			auto fi = FieldOf(KlassOf(ch), "<CharacterID>k__BackingField");
			if (!fi) return -1;
			try { return il2cpp_symbols::read_field<int>(ch, fi); }
			catch (...) { return -1; }
		}

		void* LipControllerOf(void* ch) {
			if (!ch) return nullptr;
			auto fi = FieldOf(KlassOf(ch), "_lipSyncController");
			if (!fi) return nullptr;
			try { return il2cpp_symbols::read_field<void*>(ch, fi); }
			catch (...) { return nullptr; }
		}

		void* LipTableOf(void* controller) {
			if (!controller) return nullptr;
			auto fi = FieldOf(KlassOf(controller), "_lipSyncTable");
			if (!fi) return nullptr;
			try { return il2cpp_symbols::read_field<void*>(controller, fi); }
			catch (...) { return nullptr; }
		}

		HOOK_ORIG_TYPE Character_SetupLip_orig;
		void Character_SetupLip_hook(void* _this, void* lipTable, float arg1, float arg2, void* mtd) {
			const std::string lipMode = ReadLipMode();
			void* useTable = lipTable;
			bool reused = false;
			if (lipMode == "on" || lipMode == "force") {
				std::lock_guard<std::mutex> lock(g_lipMutex);
				const bool valid = lipTable && RunwayDump::PlausiblePtr((uintptr_t)lipTable);
				// Prefer a table captured outside solo mode; only fall back to
				// caching a solo table when this song has no cached table yet.
				// Never overwrite a good normal-mode table with a solo one.
				const bool haveForSong = g_lipCachedHandle && g_lipCachedSong == g_lastSongId;
				if (valid && (g_lastSoloVocalMode != 2 || !haveForSong)) {
					if (g_lipCachedHandle) il2cpp_gchandle_free(g_lipCachedHandle);
					g_lipCachedHandle = il2cpp_gchandle_new((Il2CppObject*)lipTable, false);
					g_lipCachedSong = g_lastSongId;
				}
				else if (!lipTable && g_lipCachedHandle && g_lastSongId > 0 &&
					g_lipCachedSong == g_lastSongId) {
					void* cached = il2cpp_gchandle_get_target(g_lipCachedHandle);
					if (cached && RunwayDump::PlausiblePtr((uintptr_t)cached)) {
						useTable = cached;
						reused = true;
						++g_lipHookSubs;
					}
				}
			}
			const int call = ++g_lipHookCalls;
			if (call <= 200) {
				Log(std::format("[v33] SetupLip song={} vmMode={} lip={} this={} in={} use={} inputNull={} reused={} a1={} a2={}",
					g_lastSongId, g_lastSoloVocalMode, lipMode, ViewDiag::ptr_str(_this),
					ViewDiag::ptr_str((void*)lipTable), ViewDiag::ptr_str(useTable),
					lipTable ? 0 : 1, reused ? 1 : 0, arg1, arg2));
			}
			return HOOK_CAST_CALL(void, Character_SetupLip)(_this, useTable, arg1, arg2, mtd);
		}

		HOOK_ORIG_TYPE Character_UpdateLip_orig;
		HOOK_ORIG_TYPE LipSyncController_UpdateLip_orig;
		int g_lipUpdateCalls = 0;

		bool LipPatchController(void* controller, const char* source) {
			if (!controller) return false;
			auto k = KlassOf(controller);
			if (!k) return false;
			auto fi = FieldOf(k, "_lipSyncTable");
			if (!fi) return false;
			void* table = nullptr;
			try { table = il2cpp_symbols::read_field<void*>(controller, fi); }
			catch (...) { table = nullptr; }

			const int call = ++g_lipUpdateCalls;
			static std::string cachedMode;
			static int modeReadAt = -1000;
			if (call - modeReadAt >= 30) {
				modeReadAt = call;
				cachedMode = ReadLipMode();
			}
			const std::string& mode = cachedMode;
			bool patched = false;
			if (mode == "on" || mode == "force") {
				std::lock_guard<std::mutex> lock(g_lipMutex);
				const bool valid = table && RunwayDump::PlausiblePtr((uintptr_t)table);
				const bool haveForSong = g_lipCachedHandle && g_lipCachedSong == g_lastSongId;
				if (valid && (g_lastSoloVocalMode != 2 || !haveForSong)) {
					if (g_lipCachedHandle) il2cpp_gchandle_free(g_lipCachedHandle);
					g_lipCachedHandle = il2cpp_gchandle_new((Il2CppObject*)table, false);
					g_lipCachedSong = g_lastSongId;
				}
				else if (!table && g_lipCachedHandle && g_lastSongId > 0 &&
					g_lipCachedSong == g_lastSongId) {
					void* cached = il2cpp_gchandle_get_target(g_lipCachedHandle);
					if (cached && RunwayDump::PlausiblePtr((uintptr_t)cached)) {
						void* value = cached;
						il2cpp_field_set_value(controller, fi, &value);
						patched = true;
					}
				}
			}
			if (call <= 40 || patched) {
				Log(std::format("[v34] LipUpdate src={} song={} vmMode={} lip={} this={} table={} patched={}",
					source, g_lastSongId, g_lastSoloVocalMode, mode,
					ViewDiag::ptr_str(controller), ViewDiag::ptr_str(table), patched ? 1 : 0));
			}
			return patched || table != nullptr;
		}

		void Character_UpdateLip_hook(void* _this, float dt, void* mtd) {
			try {
				if (_this) {
					auto fi = FieldOf(KlassOf(_this), "_lipSyncController");
					if (fi) {
						void* controller = nullptr;
						try { controller = il2cpp_symbols::read_field<void*>(_this, fi); }
						catch (...) { controller = nullptr; }
						LipPatchController(controller, "Character.UpdateLip");
					}
				}
			}
			catch (...) {}
			return HOOK_CAST_CALL(void, Character_UpdateLip)(_this, dt, mtd);
		}

		void LipSyncController_UpdateLip_hook(void* _this, float dt, void* mtd) {
			try { LipPatchController(_this, "LipSyncController.UpdateLip"); } catch (...) {}
			return HOOK_CAST_CALL(void, LipSyncController_UpdateLip)(_this, dt, mtd);
		}

		HOOK_ORIG_TYPE Character_SetAutoUpdateStop_orig;
		void Character_SetAutoUpdateStop_hook(void* _this, bool stop, void* mtd) {
			const bool use = g_lipForceMode ? false : stop;
			if (g_lipFlagLogs++ < 160) {
				Log(std::format("[v35] SetAutoUpdateStop ch={} in={} use={} force={} field={}",
					LipCharaId(_this), stop ? 1 : 0, use ? 1 : 0, g_lipForceMode ? 1 : 0,
					LipReadBool(_this, "isStopUpdate") ? 1 : 0));
			}
			return HOOK_CAST_CALL(void, Character_SetAutoUpdateStop)(_this, use, mtd);
		}

		HOOK_ORIG_TYPE Character_SetOverwriteFlag_orig;
		void Character_SetOverwriteFlag_hook(void* _this, bool flag, void* mtd) {
			const bool use = g_lipForceMode ? true : flag;
			if (g_lipFlagLogs++ < 160) {
				Log(std::format("[v35] SetOverwriteFlag ch={} in={} use={} force={} field={}",
					LipCharaId(_this), flag ? 1 : 0, use ? 1 : 0, g_lipForceMode ? 1 : 0,
					LipReadBool(_this, "animationOverwrite") ? 1 : 0));
			}
			return HOOK_CAST_CALL(void, Character_SetOverwriteFlag)(_this, use, mtd);
		}

		HOOK_ORIG_TYPE LipSyncController_SetSerious_orig;
		void LipSyncController_SetSerious_hook(void* _this, bool serious, void* mtd) {
			const bool use = g_lipForceMode ? true : serious;
			if (g_lipFlagLogs++ < 160) {
				Log(std::format("[v35] SetSerious in={} use={} force={} field={}",
					serious ? 1 : 0, use ? 1 : 0, g_lipForceMode ? 1 : 0,
					LipReadBool(_this, "isSerious") ? 1 : 0));
			}
			return HOOK_CAST_CALL(void, LipSyncController_SetSerious)(_this, use, mtd);
		}

		HOOK_ORIG_TYPE LipSyncController_SetOverwrite_orig;
		void LipSyncController_SetOverwrite_hook(void* _this, bool overwrite, void* mtd) {
			const bool use = g_lipForceMode ? true : overwrite;
			if (g_lipFlagLogs++ < 160) {
				Log(std::format("[v35] SetOverwrite in={} use={} force={} field={}",
					overwrite ? 1 : 0, use ? 1 : 0, g_lipForceMode ? 1 : 0,
					LipReadBool(_this, "isOverwrite") ? 1 : 0));
			}
			return HOOK_CAST_CALL(void, LipSyncController_SetOverwrite)(_this, use, mtd);
		}

		HOOK_ORIG_TYPE Character_ResetLipSync_orig;
		void Character_ResetLipSync_hook(void* _this, void* mtd) {
			if (g_lipFlagLogs++ < 160)
				Log(std::format("[v35] Character.ResetLipSync ch={} table={}",
					LipCharaId(_this), ViewDiag::ptr_str(LipTableOf(LipControllerOf(_this)))));
			return HOOK_CAST_CALL(void, Character_ResetLipSync)(_this, mtd);
		}

		HOOK_ORIG_TYPE LiveCharacterController_ResetLipSync_orig;
		void LiveCharacterController_ResetLipSync_hook(void* _this, void* mtd) {
			if (g_lipFlagLogs++ < 160)
				Log("[v35] LiveCharacterController.ResetLipSync");
			return HOOK_CAST_CALL(void, LiveCharacterController_ResetLipSync)(_this, mtd);
		}

		HOOK_ORIG_TYPE Character_Update_orig;
		void Character_Update_hook(void* _this, void* mtd) {
			static int lastMode = -2, n = 0;
			static std::unordered_set<uintptr_t> patchedThisMode;
			if (g_lastSoloVocalMode != lastMode) {
				lastMode = g_lastSoloVocalMode;
				n = 0;
				patchedThisMode.clear();
			}
			if (_this && g_lastSoloVocalMode == 2 &&
				patchedThisMode.find((uintptr_t)_this) == patchedThisMode.end()) {
				auto controller = LipControllerOf(_this);
				if (controller && LipPatchController(controller, "Character.Update"))
					patchedThisMode.insert((uintptr_t)_this);
			}
			if (++n <= 10) {
				Log(std::format("[v35] Character.Update ch={} vmMode={} stop={} recorded={} overwrite={} table={}",
					LipCharaId(_this), g_lastSoloVocalMode,
					LipReadBool(_this, "isStopUpdate") ? 1 : 0,
					LipReadBool(_this, "isRecordedAnimation") ? 1 : 0,
					LipReadBool(_this, "animationOverwrite") ? 1 : 0,
					ViewDiag::ptr_str(LipTableOf(LipControllerOf(_this)))));
			}
			return HOOK_CAST_CALL(void, Character_Update)(_this, mtd);
		}

		void InstallLipHook(int& installed) {
			const std::string mode = ReadLipMode();
			if (mode != "diag" && mode != "on" && mode != "force") {
				Log(std::format("[v34] lip hook off (lip.txt='{}')", mode));
				return;
			}
			g_lipForceMode = (mode == "force");
			const char* assemblies[] = { "PRISM.Legacy.dll", "PRISM.dll" };
			bool charDone = false, ctrlDone = false;
			for (const char* a : assemblies) {
				if (!charDone) {
					auto k = il2cpp_symbols_logged::get_class(a, "PRISM", "Character");
					auto mi = k ? il2cpp_class_get_method_from_name(k, "UpdateLip", 1) : nullptr;
					if (mi && mi->methodPointer) {
						const char* tn = il2cpp_method_get_param_count(mi) > 0
							? il2cpp_symbols::il2cpp_method_get_param_type_name(mi, 0) : "?";
						Log(std::format("[v34] {}.PRISM.Character::UpdateLip({}) @ {}",
							a, tn ? tn : "?", ViewDiag::ptr_str((void*)mi->methodPointer)));
						if (HookAt("Character.UpdateLip", (void*)mi->methodPointer,
							(void*)Character_UpdateLip_hook, Character_UpdateLip_orig)) {
							++installed;
							charDone = true;
						}
					}
				}
				if (!ctrlDone) {
					auto k = il2cpp_symbols_logged::get_class(a, "PRISM", "LipSyncController");
					auto mi = k ? il2cpp_class_get_method_from_name(k, "UpdateLip", 1) : nullptr;
					if (mi && mi->methodPointer) {
						const char* tn = il2cpp_method_get_param_count(mi) > 0
							? il2cpp_symbols::il2cpp_method_get_param_type_name(mi, 0) : "?";
						Log(std::format("[v34] {}.PRISM.LipSyncController::UpdateLip({}) @ {}",
							a, tn ? tn : "?", ViewDiag::ptr_str((void*)mi->methodPointer)));
						if (HookAt("LipSyncController.UpdateLip", (void*)mi->methodPointer,
							(void*)LipSyncController_UpdateLip_hook, LipSyncController_UpdateLip_orig)) {
							++installed;
							ctrlDone = true;
						}
					}
				}
				if (charDone && ctrlDone) break;
			}
			int flagHooks = 0;
			auto addFlagHook = [&](const char* cls, const char* name, int argc,
				void* detour, HOOK_ORIG_TYPE& orig, const char* label) {
				auto k = il2cpp_symbols_logged::get_class("PRISM.Legacy.dll", "PRISM", cls);
				if (!k) { Log(std::format("[v35] class {} not found", cls)); return; }
				auto mi = il2cpp_class_get_method_from_name(k, name, argc);
				if (!mi || !mi->methodPointer) {
					Log(std::format("[v35] {}::{}({}) not found", cls, name, argc));
					return;
				}
				if (HookAt(label, (void*)mi->methodPointer, detour, orig)) {
					++installed;
					++flagHooks;
					Log(std::format("[v35] {}::{}({}) hooked", cls, name, argc));
				}
			};
			addFlagHook("Character", "SetAutoUpdateStop", 1,
				(void*)Character_SetAutoUpdateStop_hook, Character_SetAutoUpdateStop_orig,
				"Character.SetAutoUpdateStop");
			addFlagHook("Character", "SetOverwriteFlag", 1,
				(void*)Character_SetOverwriteFlag_hook, Character_SetOverwriteFlag_orig,
				"Character.SetOverwriteFlag");
			addFlagHook("Character", "ResetLipSync", 0,
				(void*)Character_ResetLipSync_hook, Character_ResetLipSync_orig,
				"Character.ResetLipSync");
			addFlagHook("Character", "Update", 0,
				(void*)Character_Update_hook, Character_Update_orig,
				"Character.Update");
			addFlagHook("LipSyncController", "SetSerious", 1,
				(void*)LipSyncController_SetSerious_hook, LipSyncController_SetSerious_orig,
				"LipSyncController.SetSerious");
			addFlagHook("LipSyncController", "SetOverwrite", 1,
				(void*)LipSyncController_SetOverwrite_hook, LipSyncController_SetOverwrite_orig,
				"LipSyncController.SetOverwrite");
			addFlagHook("LiveCharacterController", "ResetLipSync", 0,
				(void*)LiveCharacterController_ResetLipSync_hook, LiveCharacterController_ResetLipSync_orig,
				"LiveCharacterController.ResetLipSync");
			Log(std::format("[v35] lip flag hooks={} force={} mode={}",
				flagHooks, g_lipForceMode ? 1 : 0, mode));
			Log(std::format("[v34] lip update hooks installed: Character={} Controller={} mode={}",
				charDone ? 1 : 0, ctrlDone ? 1 : 0, mode));
		}


		void Install() {
			const char* e1 = getenv("SCSP_SOLO_UNLOCK");
			if (e1 && *e1 == '0') {
				g_enabled = false;
				Log("[v24] SCSP_SOLO_UNLOCK=0 -> vocal-separated unlock disabled, nothing installed");
				return;
			}
			const char* e2 = getenv("SCSP_SOLO_ALL");
			if (e2 && *e2 == '1') g_forceAll = true;
			const char* e3 = getenv("SCSP_SOLO_ALL_SONGS");
			if (e3 && *e3 == '1') g_allSongs = true;

			Log(std::format("[v27] installing solo unlock (master-field write + v24 detours): allSongs={} forceAll={} pid={}",
				g_allSongs ? 1 : 0, g_forceAll ? 1 : 0, GetCurrentProcessId()));

			int installed = 0;
			installed += HookAt("MusicData.get_IsLimitedVocalSeparatedLocked",
				MethodAddr("PRISM.Legacy.dll", "PRISM.Live", "MusicData", "get_IsLimitedVocalSeparatedLocked", 0),
				(void*)SoloLocked_get_hook, SoloLocked_get_orig) ? 1 : 0;
			installed += HookAt("MusicData.IsLimitedVocalSeparatedLocked",
				MethodAddr("PRISM.Legacy.dll", "PRISM.Live", "MusicData", "IsLimitedVocalSeparatedLocked", 1),
				(void*)SoloLocked_arg_hook, SoloLocked_arg_orig) ? 1 : 0;
			installed += HookAt("MusicData.CheckLimitedVocalSeparatedSatisfy",
				MethodAddr("PRISM.Legacy.dll", "PRISM.Live", "MusicData", "CheckLimitedVocalSeparatedSatisfy", 1),
				(void*)SoloSatisfy_music_hook, SoloSatisfy_music_orig) ? 1 : 0;
			installed += HookAt("MstSong.CheckLimitedVocalSeparatedSatisfy",
				MethodAddr("PRISM.Definitions.dll", "PRISM.Definitions", "MstSong", "CheckLimitedVocalSeparatedSatisfy", 0),
				(void*)SoloSatisfy_mst_hook, SoloSatisfy_mst_orig) ? 1 : 0;
			installed += HookAt("MstSong.get_IsSongPartsSolo",
				MethodAddr("PRISM.Definitions.dll", "PRISM.Definitions", "MstSong", "get_IsSongPartsSolo", 0),
				(void*)MstSong_get_IsSongPartsSolo_hook, MstSong_get_IsSongPartsSolo_orig) ? 1 : 0;
			installed += HookAt("MusicData.GetValidVocalSeparatedMode",
				MethodAddr("PRISM.Legacy.dll", "PRISM.Live", "MusicData", "GetValidVocalSeparatedMode", 0),
				(void*)MusicData_GetValidVocalSeparatedMode_hook, MusicData_GetValidVocalSeparatedMode_orig) ? 1 : 0;
			if (g_forceAll) {
				installed += HookAt("MusicData.get_IsAllCharacterVocalSeparated",
					MethodAddr("PRISM.Legacy.dll", "PRISM.Live", "MusicData", "get_IsAllCharacterVocalSeparated", 0),
					(void*)SoloAllCharas_hook, SoloAllCharas_orig) ? 1 : 0;
			}
			InstallLipHook(installed);   // [v29] lip-table fallback (signature-gated)
			Log(std::format("[v27] {} hook(s) installed", installed));
		}

		void Tick() {
			static int tries = 0;
			static int tableTries = 0;
			if (!g_installed) {
				if (++tries < 3) return;   // let the il2cpp domain settle first
				if (tries > 600) { g_installed = true; return; }
				if (!il2cpp_symbols_logged::get_class("PRISM.Definitions.dll", "PRISM.Definitions", "MstSong"))
					return;
				g_installed = true;
				try { Install(); }
				catch (...) { Log("[v24] Install threw"); }
				try { DumpClassFieldsOnce(); }
				catch (...) { Log("[v24] DumpClassFieldsOnce threw"); }
				return;
			}
			if (g_tablesDumped && g_songsPatched) return;
			if (++tableTries > 3600) { g_tablesDumped = true; g_songsPatched = true; return; }
			if ((tableTries % 60) != 1) return;   // one probe per second, no busy loop
			if (!g_songsPatched) {
				try { PatchAllSongs(); }
				catch (...) { Log("[v26] PatchAllSongs threw"); }
			}
			if (!g_tablesDumped) {
				try { DumpPartsCharactersOnce(); }
				catch (...) { Log("[v24] DumpPartsCharactersOnce threw"); }
			}
		}
	}





	void MainThreadDispatcher_LateUpdate_hook(void* _this, void* method) {
		if (!g_saveAuditScanComplete) {
			g_saveAuditScanComplete = true;
			DumpSaveAuditMethodSignatures();
		}
		try {
			auto it = mainThreadTasks.begin();
			while (it != mainThreadTasks.end()) {
				try {
					bool result = (*it)();
					if (result) {
						it = mainThreadTasks.erase(it);
					}
					else {
						++it;
					}
				}
				catch (std::exception& e) {
					printf("MainThreadDispatcher Tasks Error: %s\n", e.what());
					it = mainThreadTasks.erase(it);
				}
			}
		}
		catch (std::exception& ex) {
			printf("MainThreadDispatcher Error: %s\n", ex.what());
		}
		OverrideAllTransforms();
		EventDates::TryDumpTable();  // [PATCH] v14 - master dump + one-shot "never ends" date patch
		RunwayDump::TryDumpMasters();  // [DIAG] v21 - one-shot runway master dump
		RunwaySceneScan::TickSceneScan();  // [DIAG] v23 - runway scene texture/TMP discovery
		ViewDiag::Tick();            // [DIAG] v15 - periodic viewMapping dumps
		SoloUnlock::Tick();          // [PATCH] v24 - unlock every song's 分唱/solo parts
		return HOOK_CAST_CALL(void, MainThreadDispatcher_LateUpdate)(_this, method);
	}

	// =====================================================================
	// [DIAG] v17 - unlock decision tracing.
	//   Records how many statuses the reply hooks force to isUnlocked_=true
	//   and lets CostumePartViewModel log what the UI actually sees while
	//   it builds its parts (incl. the raw status object it holds).  All
	//   lines go to unlock-diag.txt in the game root.
	// =====================================================================
	namespace UnlockDiag {
		int g_forcedCos = 0, g_forcedHair = 0, g_forcedAcc = 0;
		int g_trueCos = 0, g_trueHair = 0, g_trueAcc = 0;
		int g_partCalls = 0, g_partLogs = 0;
		int g_ctorLogs = 0, g_svcLogs = 0;
		int g_svcProbe = 0;
		// [PATCH] v20
		int g_accCtorLogs = 0, g_decide0Logs = 0, g_decide2Logs = 0, g_footLogs = 0;

		std::string objDesc(void* obj) {
			if (!obj) return "(null)";
			try {
				auto k = il2cpp_symbols::get_class_from_instance(obj);
				const char* ns = il2cpp_class_get_namespace(k);
				const char* nm = il2cpp_class_get_name(k);
				auto f = il2cpp_class_get_field_from_name(k, "isUnlocked_");
				const bool u = f ? il2cpp_symbols::read_field<bool>(obj, f) : false;
				return std::format("{}.{}(isUnlocked_={})", ns && *ns ? ns : "?", nm ? nm : "?", u ? 1 : 0);
			}
			catch (...) { return "(err)"; }
		}

		int objId(void* obj) {
			if (!obj) return 0;
			try {
				auto k = il2cpp_symbols::get_class_from_instance(obj);
				for (const char* name : { "get_MstCostumeId", "get_MstHairstyleId", "get_MstAccessoryId", "get_Id" }) {
					auto m = il2cpp_class_get_method_from_name(k, name, 0);
					if (m && m->methodPointer) {
						return reinterpret_cast<int(*)(void*)>(m->methodPointer)(obj);
					}
				}
			}
			catch (...) {}
			return 0;
		}

		// [DIAG+PATCH] v18 - dump the cached costume service list contents and
		// force every item's isUnlocked_ to true in place.
		void DumpAndForceList(void* list, const char* label) {
			if (!list) { ViewDiag::appendLog("unlock-diag.txt", std::format("cached {} list=null", label)); return; }
			int total = 0, unlockedBefore = 0;
			std::string lockedSamples;
			il2cpp_symbols::iterate_IEnumerable(list, [&](void* item) {
				total++;
				auto k = il2cpp_symbols::get_class_from_instance(item);
				auto f = il2cpp_class_get_field_from_name(k, "isUnlocked_");
				const bool u = f ? il2cpp_symbols::read_field<bool>(item, f) : false;
				if (u) { unlockedBefore++; return; }
				if (lockedSamples.size() < 220 && total < 2000) lockedSamples += std::format(" [{} {}]", objId(item), objDesc(item));
				if (f) il2cpp_symbols::write_field(item, f, true);
			});
			ViewDiag::appendLog("unlock-diag.txt", std::format("cached {} total={} unlockedBefore={} lockedSamples:{}", label, total, unlockedBefore, lockedSamples));
		}
	}

	// [PATCH] __OBSOLETE_HOOK__ guard removed - THIS IS THE REAL UNLOCK.
	// checkAndAddCostume() writes isUnlocked_ = true on every CostumeStatus the client
	// receives, and the GetCostumeListReply_get_* hooks below feed it the whole list.
	void checkAndAddCostume(int key, void* value) {
		static auto CostumeStatus_klass = il2cpp_symbols::get_class("PRISM.Module.Networking.dll",
			"PRISM.Module.Networking.Stub.Status", "CostumeStatus");
		static auto HairstyleStatus_klass = il2cpp_symbols::get_class("PRISM.Module.Networking.dll",
			"PRISM.Module.Networking.Stub.Status", "HairstyleStatus");
		static auto AccessoryStatus_klass = il2cpp_symbols::get_class("PRISM.Module.Networking.dll",
			"PRISM.Module.Networking.Stub.Status", "AccessoryStatus");
		auto value_klass = il2cpp_symbols::get_class_from_instance(value);

#define unlockAndAddToMap(klass, cacheMap, tag) \
		static auto klass##_isUnlocked_field = il2cpp_class_get_field_from_name(klass, "isUnlocked_"); \
		if (il2cpp_symbols::read_field<bool>(value, klass##_isUnlocked_field)) UnlockDiag::g_true##tag++; \
		else UnlockDiag::g_forced##tag++; \
		il2cpp_symbols::write_field(value, klass##_isUnlocked_field, true); \
		cacheMap.emplace(key, value)

		if (value_klass == CostumeStatus_klass) {
			// wprintf(L"dic_int_ICostumeStatus_add: this: %p, key: %d\n", _this, key);
			unlockAndAddToMap(CostumeStatus_klass, cacheDressMap, Cos);
		}
		else if (value_klass == HairstyleStatus_klass) {
			if (g_unlock_all_headwear) {
				unlockAndAddToMap(HairstyleStatus_klass, cacheHairMap, Hair);
			}
		}
		else if (value_klass == AccessoryStatus_klass) {
			if (g_unlock_all_headwear) {
				unlockAndAddToMap(AccessoryStatus_klass, cacheAccessoryMap, Acc);
			}
		}
	}

	HOOK_ORIG_TYPE dic_int_ICostumeStatus_add_orig;
	void dic_int_ICostumeStatus_add_hook(void* _this, int key, void* value, MethodInfo* method) {  // 添加服装到缓存表(失效)
		if ((g_unlock_all_dress || g_allow_use_tryon_costume) && confirmationingModel) checkAndAddCostume(key, value);
		return HOOK_CAST_CALL(void, dic_int_ICostumeStatus_add)(_this, key, value, method);
	}

	void checkCostumeListReply(void* _this, void* ret, const char* costumeIdFieldName) {
		if (g_unlock_all_dress || g_allow_use_tryon_costume) {
			static auto GetCostumeListReply_klass = il2cpp_symbols::get_class("PRISM.Module.Networking.dll",
				"PRISM.Module.Networking.Stub.Api", "GetCostumeListReply");
			auto this_klass = il2cpp_symbols::get_class_from_instance(_this);

			if (this_klass == GetCostumeListReply_klass) {
				void* CostumeStatus_klass = NULL;
				FieldInfo* mstCostumeId_field;
				int itemCount = 0;

				if (ret) {
					Utils::CSListEditor retList(ret);
					for (auto i : retList) {
						if (CostumeStatus_klass == NULL) {
							CostumeStatus_klass = il2cpp_symbols::get_class_from_instance(i);
							mstCostumeId_field = il2cpp_class_get_field_from_name(CostumeStatus_klass, costumeIdFieldName);
						}
						const auto mstCostumeId = il2cpp_symbols::read_field<int>(i, mstCostumeId_field);
						checkAndAddCostume(mstCostumeId, i);
						itemCount++;
					}
				}
				ViewDiag::appendLog("unlock-diag.txt", std::format(
					"reply-list field={} items={} forced(cos/hair/acc)={}/{}/{} alreadyTrue={}/{}/{}",
					costumeIdFieldName, itemCount,
					UnlockDiag::g_forcedCos, UnlockDiag::g_forcedHair, UnlockDiag::g_forcedAcc,
					UnlockDiag::g_trueCos, UnlockDiag::g_trueHair, UnlockDiag::g_trueAcc));
			}
		}
	}

	HOOK_ORIG_TYPE GetCostumeListReply_get_CostumeList_orig;
	void* GetCostumeListReply_get_CostumeList_hook(void* _this) {  // 添加服装到缓存表
		auto ret = HOOK_CAST_CALL(void*, GetCostumeListReply_get_CostumeList)(_this);
		checkCostumeListReply(_this, ret, "mstCostumeId_");
		return ret;
	}

	// The first registered auto getter
	HOOK_ORIG_TYPE GetCostumeListReply_get_HairstyleList_orig;
	void* GetCostumeListReply_get_HairstyleList_hook(void* _this) {  // 添加服装到缓存表
		auto ret = HOOK_CAST_CALL(void*, GetCostumeListReply_get_HairstyleList)(_this);
		checkCostumeListReply(_this, ret, "mstHairstyleId_");
		return ret;
	}

	HOOK_ORIG_TYPE GetCostumeListReply_get_AccessoryList_orig;
	void* GetCostumeListReply_get_AccessoryList_hook(void* _this) {  // 添加服装到缓存表
		auto ret = HOOK_CAST_CALL(void*, GetCostumeListReply_get_AccessoryList)(_this);
		checkCostumeListReply(_this, ret, "mstAccessoryId_");
		return ret;
	}
	// [PATCH] #endif removed (matching the #ifdef __OBSOLETE_HOOK__ that was deleted above)

	// [DIAG] v17 - what the costume-change UI sees per part.
	// [PATCH] v18 - accessory parts (2..7) are forced to Unlocked for display.
	// (v18's cache-service probe was removed in v19: invoking those service
	//  methods through raw method pointers crashed the client.)
	HOOK_ORIG_TYPE CostumePartViewModel_getStatus_orig;
	int CostumePartViewModel_getStatus_hook(void* _this, bool isUnlocked, void* unlockableIds) {
		const auto ret = HOOK_CAST_CALL(int, CostumePartViewModel_getStatus)(_this, isUnlocked, unlockableIds);
		int out_ret = ret;
		try {
			static auto part_klass = ViewDiag::find_class_anywhere("PRISM.Adapters.CostumeChange", "CostumePartViewModel");
			static auto type_field = part_klass ? il2cpp_class_get_field_from_name((Il2CppClass*)part_klass, "CostumePartType") : nullptr;
			static auto isDefault_field = part_klass ? il2cpp_class_get_field_from_name((Il2CppClass*)part_klass, "IsDefault") : nullptr;
			static auto raw_field = part_klass ? il2cpp_class_get_field_from_name((Il2CppClass*)part_klass, "rawStatus") : nullptr;
			static auto master_field = part_klass ? il2cpp_class_get_field_from_name((Il2CppClass*)part_klass, "Master") : nullptr;
			const int partType = type_field ? il2cpp_symbols::read_field<int>(_this, type_field) : -1;
			if (ret != 0 && partType >= 2 && partType <= 7) out_ret = 0;
			const int callNo = UnlockDiag::g_partCalls++;
			const bool interesting = ret != 0;   // 1=NotDressOrdered 2=Unlockable 3=Locked
			if (UnlockDiag::g_partLogs < 800 || (interesting && UnlockDiag::g_partLogs < 6000)) {
				UnlockDiag::g_partLogs++;
				const bool isDefault = isDefault_field ? il2cpp_symbols::read_field<bool>(_this, isDefault_field) : false;
				void* raw = raw_field ? il2cpp_symbols::read_field<void*>(_this, raw_field) : nullptr;
				void* master = master_field ? il2cpp_symbols::read_field<void*>(_this, master_field) : nullptr;
				ViewDiag::appendLog("unlock-diag.txt", std::format(
					"part#{} type={} unlockedArg={} status={} forced={} isDefault={} id={} raw={}",
					callNo, partType, isUnlocked ? 1 : 0, ret, out_ret, isDefault ? 1 : 0,
					UnlockDiag::objId(master), UnlockDiag::objDesc(raw)));
			}
		}
		catch (...) {}
		return out_ret;
	}

	// [DIAG] v18/v19 - the cached-service probe (DumpAndForceList) is currently
	// NOT called because invoking the service methods crashed the client.


	// [DIAG] v17 - fallback probe: the costume part .ctor (5 params) directly.
	HOOK_ORIG_TYPE CostumePartViewModel_ctor5_orig;
	void CostumePartViewModel_ctor5_hook(void* _this, void* status, int characterId, int defaultCostumeId, void* unlockableIds, bool isAllDressOrdered) {
		HOOK_CAST_CALL(void, CostumePartViewModel_ctor5)(_this, status, characterId, defaultCostumeId, unlockableIds, isAllDressOrdered);
		try {
			if (UnlockDiag::g_ctorLogs < 40) {
				UnlockDiag::g_ctorLogs++;
				const int sid = UnlockDiag::objId(status);
				void* cached = nullptr;
				{
					auto it = cacheDressMap.find(sid);
					if (it != cacheDressMap.end()) cached = it->second;
				}
				ViewDiag::appendLog("unlock-diag.txt", std::format(
					"ctor5 #{} status={} id={} ptrMatchesCache={} isAllDressOrdered={} charId={} defaultId={}",
					UnlockDiag::g_ctorLogs, UnlockDiag::objDesc(status), sid,
					(cached && cached == status) ? 1 : 0, isAllDressOrdered ? 1 : 0, characterId, defaultCostumeId));
			}
		}
		catch (...) {}
		return;
	}

	// [PATCH] v20 - accessory parts (CostumePartType 2..7) are constructed from
	// an accessory status whose isUnlocked flag reads false, so their Status
	// field starts as Unlockable/Locked and the costume-change footer shows the
	// "解放" (unlock) button instead of "決定".  We force the field to 0
	// (Unlocked) right after construction so every reader - icon badges, the
	// footer button, decide checks - sees the part as unlocked.
	HOOK_ORIG_TYPE CostumePartViewModel_ctorAcc_orig;
	void CostumePartViewModel_ctorAcc_hook(void* _this, void* status, void* defaultAccessoryIds, void* unlockableAccessoryIds) {
		HOOK_CAST_CALL(void, CostumePartViewModel_ctorAcc)(_this, status, defaultAccessoryIds, unlockableAccessoryIds);
		try {
			static auto part_klass = ViewDiag::find_class_anywhere("PRISM.Adapters.CostumeChange", "CostumePartViewModel");
			static auto type_field = part_klass ? il2cpp_class_get_field_from_name((Il2CppClass*)part_klass, "CostumePartType") : nullptr;
			static auto status_field = part_klass ? il2cpp_class_get_field_from_name((Il2CppClass*)part_klass, "Status") : nullptr;
			const int partType = type_field ? il2cpp_symbols::read_field<int>(_this, type_field) : -1;
			const int st = status_field ? il2cpp_symbols::read_field<int>(_this, status_field) : 0;
			if (status_field && partType >= 2 && partType <= 7 && st != 0) {
				il2cpp_symbols::write_field(_this, status_field, 0);
				if (UnlockDiag::g_accCtorLogs < 60) {
					UnlockDiag::g_accCtorLogs++;
					ViewDiag::appendLog("unlock-diag.txt", std::format(
						"ctorAcc #{} type={} status={}->0 id={} raw={}",
						UnlockDiag::g_accCtorLogs, partType, st,
						UnlockDiag::objId(status), UnlockDiag::objDesc(status)));
				}
			}
		}
		catch (...) {}
		return;
	}

	// =====================================================================
	// [PATCH] Master row dump v5 - ZERO I/O in the hot path.
	//
	//   v4 wrote one file line per getter call, which produced 182 MB /
	//   4.2 M lines and made the idol list screen hang forever.  v5 only
	//   records *distinct* values in memory and dumps the contiguous
	//   MstProduceIdol row array as raw bytes, once every 64 KB of newly
	//   discovered range.  Total disk traffic stays around 25 MB and no
	//   file is ever touched from inside a getter that the UI calls in a
	//   loop.
	// =====================================================================
	namespace MasterDump {
		std::mutex g_mtx;
		std::set<std::string> g_seen;        // dedupe key "what:v=N"
		std::set<int> g_idolIds;             // every distinct MstIdolId
		std::set<int> g_rowIds;              // every distinct Id
		void* g_lo = nullptr;                // lowest row address seen
		void* g_hi = nullptr;                // highest row address seen
		size_t g_dumped = 0;                 // range already written out

		// Record a distinct value.  Returns true if it is new.
		bool Note(const char* what, bool isIdolId, int value) {
			if (value <= 0) return false;
			std::lock_guard<std::mutex> lk(g_mtx);
			char key[80];
			snprintf(key, sizeof(key), "%s:v=%d", what, value);
			if (!g_seen.insert(key).second) return false;
			(isIdolId ? g_idolIds : g_rowIds).insert(value);
			std::ofstream f("master-dump.txt", std::ios::app);
			if (f.is_open()) f << what << " v=" << value << "\n";
			return true;
		}

		// Track the row address.  Returns true when the discovered range
		// grew by at least 64 KB since the last dump (so DumpRows is due).
		bool Track(void* row) {
			if (!row) return false;
			const uintptr_t r = (uintptr_t)row;
			std::lock_guard<std::mutex> lk(g_mtx);
			const uintptr_t lo = (uintptr_t)g_lo;
			const uintptr_t hi = (uintptr_t)g_hi;
			if (!g_lo || r < lo) g_lo = row;
			if (!g_hi || r > hi) g_hi = row;
			if (!g_lo || !g_hi) return false;
			const uintptr_t nlo = (uintptr_t)g_lo;
			const uintptr_t nhi = (uintptr_t)g_hi;
			if (nhi <= nlo) return false;
			const size_t span = (size_t)(nhi - nlo);
			if (span >= g_dumped + (64u << 10)) {
				g_dumped = span;
				return true;
			}
			return false;
		}

		// Write the discovered row array as raw bytes (capped at 4 MB).
		void DumpRows() {
			void* lo = nullptr;
			size_t n = 0;
			{
				std::lock_guard<std::mutex> lk(g_mtx);
				if (!g_lo || !g_hi) return;
				const uintptr_t nlo = (uintptr_t)g_lo;
				const uintptr_t nhi = (uintptr_t)g_hi;
				if (nhi <= nlo) return;
				lo = g_lo;
				n = (size_t)(nhi - nlo) + 1024;
				const size_t CAP = 4u << 20;
				if (n > CAP) n = CAP;
			}
			std::ofstream f("master-rows.bin", std::ios::binary | std::ios::trunc);
			if (f.is_open()) {
				f.write((const char*)lo, (std::streamsize)n);
				f.flush();
			}
		}
	}

	// =====================================================================
	// [PATCH] v6 - targeted master row dump.
	//
	//   Adding a produce-idol entry for a collab character (378001 ..) makes
	//   the client reject the WHOLE reply with ErrorCode[BF65431C], which is
	//   the client's "this field does not match my own master" check (the
	//   same code the costume work hit when CostumeType was wrong).
	//
	//   So the real values have to come from the master row.  This dumps the
	//   raw bytes of the MstProduceIdol object for a few MstIdolId values
	//   whose "group" (the reply's f3.f1 / f22.f2) is known from real server
	//   data: 101 -> 1, 201 -> 2, 301 -> 3.  Diffing those against 378/379/
	//   383 shows which field carries the group, and what it is for the
	//   collab characters.
	// =====================================================================
	namespace MasterRowDump {
		std::mutex g_mtx;
		std::map<int, int> g_count;

		bool wanted(int idolId) {
			switch (idolId) {
			case 101: case 102: case 201: case 202: case 301: case 302:
			case 378: case 379: case 380: case 381: case 383: case 484:
				return true;
			default:
				return false;
			}
		}

		// at most 3 objects per MstIdolId (the variants)
		bool ShouldDump(int idolId) {
			if (!wanted(idolId)) return false;
			std::lock_guard<std::mutex> lk(g_mtx);
			int& c = g_count[idolId];
			if (c >= 3) return false;
			++c;
			return true;
		}

		void Dump(void* obj, int idolId) {
			std::ofstream f("master-rows2.txt", std::ios::app);
			if (!f.is_open()) return;
			f << "=== MstIdolId " << idolId << " ptr=" << obj << "\n";
			const unsigned char* p = (const unsigned char*)obj;
			for (int i = 0; i < 256; i += 16) {
				char line[160];
				int n = snprintf(line, sizeof(line), "  +%03d ", i);
				char ascii[17];
				ascii[16] = 0;
				for (int j = 0; j < 16; ++j) {
					const unsigned char c = p[i + j];
					n += snprintf(line + n, sizeof(line) - n, "%02X ", c);
					ascii[j] = (c >= 32 && c < 127) ? (char)c : '.';
				}
				snprintf(line + n, sizeof(line) - n, " %s", ascii);
				f << line << "\n";
			}
			f.flush();
		}
	}

	// =====================================================================
	// [PATCH] v7 - limited-time gate bypass + master field dump.
	//
	//   The collab characters (78..89) and their produce idols (378xxx..389xxx)
	//   are "limited time" (期間限定) content.  The client refuses any reply
	//   entry that points at them with ErrorCode[BF65431C] because the period
	//   is over - this is why adding their P cards crashed the character list.
	//
	//   The master rows expose IsLimitedTimeUnlock / UnlockPremise /
	//   DressOrderBeginDate (confirmed in global-metadata.dat).  The old mod
	//   NOP'ed the check at a fixed RVA (revertgate 0x03DCA0E9) which no longer
	//   matches, so instead we force the flag OFF on every master row that has
	//   it - build independent.
	// =====================================================================
#define LIMITED_TIME_GATE_HOOK(klass) \
	HOOK_ORIG_TYPE klass##_get_IsLimitedTimeUnlock_orig; \
	bool klass##_get_IsLimitedTimeUnlock_hook(void* _this) { \
		(void)HOOK_CAST_CALL(bool, klass##_get_IsLimitedTimeUnlock)(_this); \
		return false; \
	}

	LIMITED_TIME_GATE_HOOK(MstCostume)
	LIMITED_TIME_GATE_HOOK(MstHairstyle)
	LIMITED_TIME_GATE_HOOK(MstAccessory)
	LIMITED_TIME_GATE_HOOK(MstProduceIdol)
	LIMITED_TIME_GATE_HOOK(MstIdol)
	LIMITED_TIME_GATE_HOOK(MstCharacterInfo)

	// dump the method names of the interesting master classes (once)
	void DumpMasterFields() {
		static const char* kDump[] = { "MstProduceIdol", "MstIdol", "MstCharacterInfo", "MstCostume", "MstAccessory" };
		std::ofstream f("mst-fields.txt", std::ios::app);
		if (!f.is_open()) return;
		for (const char* cn : kDump) {
			f << "=== " << cn << " methods ===\n";
			auto k = il2cpp_symbols_logged::get_class("PRISM.Definitions.dll", "PRISM.Definitions", cn);
			if (!k) { f << "  (class not found)\n"; continue; }
			void* iter = nullptr;
			while (auto mi = il2cpp_class_get_methods(k, &iter)) {
				const char* mn = il2cpp_method_get_name(mi);
				f << "  " << (mn ? mn : "?") << " /" << il2cpp_method_get_param_count(mi) << "\n";
			}
		}
		// [PATCH] v8 - the networking side (error handling + the reply entry type)
		struct NetType { const char* dll; const char* ns; const char* cls; };
		static const NetType kNet[] = {
			{ "PRISM.Module.Networking.dll", "PRISM.Module.Networking", "ErrorHandler" },
			{ "PRISM.Module.Networking.dll", "PRISM.Module.Networking.Stub.Status", "ProduceIdolStatus" },
		};
		for (const auto& nt : kNet) {
			f << "=== " << nt.cls << " methods ===\n";
			auto k = il2cpp_symbols_logged::get_class(nt.dll, nt.ns, nt.cls);
			if (!k) { f << "  (class not found)\n"; continue; }
			void* iter = nullptr;
			while (auto mi = il2cpp_class_get_methods(k, &iter)) {
				const char* mn = il2cpp_method_get_name(mi);
				f << "  " << (mn ? mn : "?") << " /" << il2cpp_method_get_param_count(mi) << "\n";
			}
		}
		f.flush();
	}

	// =====================================================================
	// [PATCH] v8 - dump the master field VALUES of a few MstProduceIdol rows.
	//
	//   Adding a produce-idol entry for a collab character (378001 ..) makes the
	//   client reject the whole reply (ErrorCode[BF65431C]).  MstProduceIdol has
	//   no IsLimitedTimeUnlock field, so the check is on one of its real fields.
	//   Dumping the getters of a working row (301/302, type 3) and of the collab
	//   rows (378/379/381, whatever type they really are) shows the difference.
	// =====================================================================
	namespace MasterFieldDump {
		std::mutex g_mtx;
		std::set<int> g_done;

		bool wanted(int idolId) {
			switch (idolId) {
			case 202: case 301: case 302: case 378: case 379: case 381: case 484:
				return true;
			default:
				return false;
			}
		}

		bool ShouldDump(int idolId) {
			if (!wanted(idolId)) return false;
			std::lock_guard<std::mutex> lk(g_mtx);
			return g_done.insert(idolId).second;
		}

		void Dump(void* obj, int idolId) {
			// only real MstProduceIdol instances (compare by class NAME; the
			// class pointer from get_class() is not the same instance-cache one)
			auto inst = il2cpp_symbols::get_class_from_instance(obj);
			if (!inst) return;
			const char* clsName = il2cpp_class_get_name(inst);
			if (!clsName || strcmp(clsName, "MstProduceIdol") != 0) return;
			auto klass = inst;

			std::ofstream f("mst-values.txt", std::ios::app);
			if (!f.is_open()) return;
			f << "=== MstIdolId " << idolId << " ===\n";

			static const char* kIntGetters[] = {
				"get_Id", "get_ProduceIdolType", "get_MstIdolId", "get_InitialStar",
				"get_InitialVocal", "get_InitialDance", "get_InitialVisual", "get_InitialMental",
				"get_LimitVocal", "get_LimitDance", "get_LimitVisual", "get_LimitMental",
				"get_MaxLevelVocal", "get_MaxLevelDance", "get_MaxLevelVisual", "get_MaxLevelMental",
				"get_Attribute", "get_MstLiveCenterEffectId", "get_MstLiveSkillId",
				"get_LimitBreakRecipeProductId", "get_EvolutionRecipeGroupId",
				"get_GashaVoiceMstVoiceResourceId", "get_GashaSeMstVoiceResourceId",
				"get_SignPositionIndex",
			};
			for (const char* gn : kIntGetters) {
				auto mi = il2cpp_class_get_method_from_name(klass, gn, 0);
				if (!mi || !mi->methodPointer) { f << "  " << gn << " : (missing)\n"; continue; }
				const int v = reinterpret_cast<int(*)(void*)>(mi->methodPointer)(obj);
				f << "  " << gn << " = " << v << "\n";
			}

			// get_ReleaseDate returns a DateTime (one 8-byte ulong) -> RAX on x64
			auto rd = il2cpp_class_get_method_from_name(klass, "get_ReleaseDate", 0);
			if (rd && rd->methodPointer) {
				const long long v = reinterpret_cast<long long(*)(void*)>(rd->methodPointer)(obj);
				f << "  get_ReleaseDate(ticks) = " << v << "\n";
			}

			auto nameM = il2cpp_class_get_method_from_name(klass, "get_Name", 0);
			if (nameM && nameM->methodPointer) {
				auto s = reinterpret_cast<Il2CppString * (*)(void*)>(nameM->methodPointer)(obj);
				if (s) f << "  get_Name = " << utility::conversions::to_utf8string(std::wstring(s->start_char)) << "\n";
			}
			f.flush();
		}
	}

	// =====================================================================
	// [PATCH] v9 - capture the client's own error report.
	//
	//   Everything points at the client rejecting the reply during its own
	//   validation (ErrorCode[BF65431C] -> PRISM.Global._systemReset ->
	//   Limelight SoftReset).  PRISM.Module.Networking.ErrorHandler is the class
	//   that turns a failure into that error, so hooking it prints the real
	//   reason (class + ToString + raw bytes of the argument).
	// =====================================================================
	HOOK_ORIG_TYPE ErrorHandler_Handle_orig;
	void* ErrorHandler_Handle_hook(void* _this, void* arg, MethodInfo* method) {
		try {
			std::ofstream f("errhandler.txt", std::ios::app);
			if (f.is_open() && arg) {
				auto k = il2cpp_symbols::get_class_from_instance(arg);
				const char* kn = k ? il2cpp_class_get_name(k) : "?";
				f << "=== ErrorHandler.Handle arg class=" << (kn ? kn : "?") << "\n";
				if (k) {
					auto ts = il2cpp_class_get_method_from_name(k, "ToString", 0);
					if (ts && ts->methodPointer) {
						auto s = reinterpret_cast<Il2CppString * (*)(void*)>(ts->methodPointer)(arg);
						if (s) f << "  ToString = " << utility::conversions::to_utf8string(std::wstring(s->start_char)) << "\n";
					}
				}
				const unsigned char* p = (const unsigned char*)arg;
				for (int i = 0; i < 128; i += 16) {
					char line[160];
					int n = snprintf(line, sizeof(line), "  +%03d ", i);
					char ascii[17];
					ascii[16] = 0;
					for (int j = 0; j < 16; ++j) {
						const unsigned char c = p[i + j];
						n += snprintf(line + n, sizeof(line) - n, "%02X ", c);
						ascii[j] = (c >= 32 && c < 127) ? (char)c : '.';
					}
					snprintf(line + n, sizeof(line) - n, " %s", ascii);
					f << line << "\n";
				}
				f.flush();
			}
		}
		catch (...) {}
		return HOOK_CAST_CALL(void*, ErrorHandler_Handle)(_this, arg, method);
	}

	HOOK_ORIG_TYPE MstProduceIdol_get_Id_orig;
	int MstProduceIdol_get_Id_hook(void* _this) {
		int v = HOOK_CAST_CALL(int, MstProduceIdol_get_Id)(_this);
		if (v > 0) {
			if (MasterDump::Note("get_Id", false, v) || MasterDump::Track(_this)) {
				MasterDump::DumpRows();
			}
		}
		return v;
	}

	HOOK_ORIG_TYPE MstProduceIdol_get_MstIdolId_orig;
	int MstProduceIdol_get_MstIdolId_hook(void* _this) {
		int v = HOOK_CAST_CALL(int, MstProduceIdol_get_MstIdolId)(_this);
		if (v > 0) {
			// [PATCH] v6 targeted row dump (see MasterRowDump above)
			if (MasterRowDump::ShouldDump(v)) MasterRowDump::Dump(_this, v);
			// [PATCH] v8 field values (see MasterFieldDump above)
			if (MasterFieldDump::ShouldDump(v)) MasterFieldDump::Dump(_this, v);
			if (MasterDump::Note("get_MstIdolId", true, v) || MasterDump::Track(_this)) {
				MasterDump::DumpRows();
			}
		}
		return v;
	}

	HOOK_ORIG_TYPE MstProduceIdol_set_Id_orig;
	HOOK_ORIG_TYPE MstProduceIdol_set_MstIdolId_orig;
	void MstProduceIdol_set_Id_hook(void* _this, int value) {
		HOOK_CAST_CALL(void, MstProduceIdol_set_Id)(_this, value);
		if (value > 0) {
			if (MasterDump::Note("set_Id", false, value) || MasterDump::Track(_this)) {
				MasterDump::DumpRows();
			}
		}
	}
	void MstProduceIdol_set_MstIdolId_hook(void* _this, int value) {
		HOOK_CAST_CALL(void, MstProduceIdol_set_MstIdolId)(_this, value);
		if (value > 0) {
			if (MasterDump::Note("set_MstIdolId", true, value) || MasterDump::Track(_this)) {
				MasterDump::DumpRows();
			}
		}
	}

	HOOK_ORIG_TYPE LiveMVUnitConfirmationModel_ctor_orig;
	void LiveMVUnitConfirmationModel_ctor_hook(void* _this, void* musicData, void* saveData, void* unitListReply, void* costumeService, void* autoCoordinateFlag) {
		MstCostumeSnapshot::ResetAllRecords();
		HOOK_CAST_CALL(void, LiveMVUnitConfirmationModel_ctor)(_this, musicData, saveData, unitListReply, costumeService, autoCoordinateFlag);
		return;

		//confirmationingModel = true;
		//cacheDressMap.clear();
		//cacheHairMap.clear();
		//cacheAccessoryMap.clear();
		//
		//HOOK_CAST_CALL(void, LiveMVUnitConfirmationModel_ctor)(_this, musicData, saveData, unitListReply, costumeService);
		//confirmationingModel = false;
		//return;
	}

	HOOK_ORIG_TYPE RunwayUnitConfirmationModel_ctor_orig;
	void RunwayUnitConfirmationModel_ctor_hook(void* _this, int eventId, int stageId, void* saveData, void* unitListReply, void* costumeService, void* historyReply) {
		RunwayDump::g_lastEventId = eventId;   // [DIAG] v22
		MstCostumeSnapshot::ResetAllRecords();
		try {
			if (UnlockDiag::g_svcLogs < 4) {
				UnlockDiag::g_svcLogs++;
				ViewDiag::appendLog("unlock-diag.txt", std::format(
					"runway-model#{} ctor: svcClass={} eventId={} stageId={}",
					UnlockDiag::g_svcLogs, UnlockDiag::objDesc(costumeService), eventId, stageId));
			}
		}
		catch (...) {}
		HOOK_CAST_CALL(void, RunwayUnitConfirmationModel_ctor)(_this, eventId, stageId, saveData, unitListReply, costumeService, historyReply);
		return;
	}

	void updateSwayStringPoint(void* _this) {
		if (!g_enable_chara_param_edit) return;

		static auto SwayString_klass = il2cpp_symbols::get_class("PRISM.Legacy.dll", "PRISM", "SwayString");
		static auto swayType_field = il2cpp_class_get_field_from_name(SwayString_klass, "swayType");
		static auto swaySubType_field = il2cpp_class_get_field_from_name(SwayString_klass, "swaySubType");
		static auto forceParam_field = il2cpp_class_get_field_from_name(SwayString_klass, "forceParam");

		static auto SetTest = reinterpret_cast<void (*)(void*, int swayType, float bendStrength, float baseGravity, float inertiaMoment, float airResistance)>(
			il2cpp_symbols::get_method_pointer("PRISM.Legacy.dll", "PRISM", "SwayString", "SetupForEditor", 5)
			);
		static auto SetRate = reinterpret_cast<void (*)(void*, float)>(
			il2cpp_symbols::get_method_pointer("PRISM.Legacy.dll", "PRISM", "SwayString", "SetRate", 1)
			);
		static auto GetRate = reinterpret_cast<float (*)(void*)>(
			il2cpp_symbols::get_method_pointer("PRISM.Legacy.dll", "PRISM", "SwayString", "GetRate", 0)
			);

		static std::unordered_set<int> targetTypes{ 0x7, 0x8 };
		const auto swayType = il2cpp_symbols::read_field<int>(_this, swayType_field);
		const auto swaySubType = il2cpp_symbols::read_field<int>(_this, swaySubType_field);
		if (!charaSwayStringOffset.contains(swayType)) return;
		const auto& currConfig = charaSwayStringOffset[swayType];
		if (!currConfig.isEdited()) return;

		const auto rate = GetRate(_this);
		SetRate(_this, rate + currConfig.rate);

		auto forceParam = il2cpp_field_get_value_object(forceParam_field, _this);
		int currIndex = 0;
		il2cpp_symbols::iterate_IEnumerable(forceParam, [&](void* param) {
			if (currIndex == swayType) {
				if (!param) {
					// printf("Invalid param.\n");
					return;
				}
				static auto forceParam_klass = il2cpp_symbols::get_class_from_instance(param);
				static auto bendStrength_field = il2cpp_class_get_field_from_name(forceParam_klass, "bendStrength");
				static auto baseGravity_field = il2cpp_class_get_field_from_name(forceParam_klass, "baseGravity");
				static auto inertiaMoment_field = il2cpp_class_get_field_from_name(forceParam_klass, "inertiaMoment");
				static auto airResistance_field = il2cpp_class_get_field_from_name(forceParam_klass, "airResistance");
				static auto deformResistance_field = il2cpp_class_get_field_from_name(forceParam_klass, "deformResistance");

				const auto bendStrength = il2cpp_symbols::read_field<float>(param, bendStrength_field);
				const auto baseGravity = il2cpp_symbols::read_field<float>(param, baseGravity_field);
				const auto inertiaMoment = il2cpp_symbols::read_field<float>(param, inertiaMoment_field);
				const auto airResistance = il2cpp_symbols::read_field<float>(param, airResistance_field);
				const auto deformResistance = il2cpp_symbols::read_field<float>(param, deformResistance_field);

				printf("SwayString original: swayType: %d, swaySubType: %d, rate: %f, bendStrength: %f, baseGravity: %f, "
					"inertiaMoment: %f, airResistance: %f, deformResistance: %f\n",
					swayType, swaySubType, rate, bendStrength, baseGravity, inertiaMoment, airResistance, deformResistance);

				il2cpp_symbols::write_field(param, bendStrength_field, bendStrength + currConfig.P_bendStrength);
				il2cpp_symbols::write_field(param, baseGravity_field, baseGravity + currConfig.P_baseGravity);
				il2cpp_symbols::write_field(param, inertiaMoment_field, inertiaMoment + currConfig.P_inertiaMoment);
				il2cpp_symbols::write_field(param, airResistance_field, airResistance + currConfig.P_airResistance);
				il2cpp_symbols::write_field(param, deformResistance_field, deformResistance + currConfig.P_deformResistance);
			}

			currIndex++;
			});
	}

	HOOK_ORIG_TYPE SwayString_SetupPoint_orig;
	void SwayString_SetupPoint_hook(void* _this) {
		if (g_enable_chara_param_edit) {
			static auto SwayString_klass = il2cpp_symbols::get_class("PRISM.Legacy.dll", "PRISM", "SwayString");
			auto this_klass = il2cpp_symbols::get_class_from_instance(_this);
			if (this_klass == SwayString_klass) {
				updateSwayStringPoint(_this);
			}
		}
		HOOK_CAST_CALL(void, SwayString_SetupPoint)(_this);
	}

	HOOK_ORIG_TYPE LiveMVUnit_GetMemberChangeRequestData_orig;
	void* LiveMVUnit_GetMemberChangeRequestData_hook(void* _this, int position, void* idol, int exchangePosition) {
		if (g_allow_same_idol) {  // 此方法已过时
			exchangePosition = -1;
		}
		return HOOK_CAST_CALL(void*, LiveMVUnit_GetMemberChangeRequestData)(_this, position, idol, exchangePosition);
	}

	int slotNewCount = 0;
	bool settingLiveIdolSlot = false;
	void* lastIdol = NULL;

	HOOK_ORIG_TYPE MvUnitSlotGenerator_NewMvUnitSlot_orig;
	void* MvUnitSlotGenerator_NewMvUnitSlot_hook(int slot, void* idol, void* method) {
		if (g_allow_same_idol && settingLiveIdolSlot) {
			if (slotNewCount >= 1) {
				printf("catch exchange.\n");
				idol = lastIdol;
			}
			else {
				slotNewCount++;
				lastIdol = idol;
			}
		}
		return HOOK_CAST_CALL(void*, MvUnitSlotGenerator_NewMvUnitSlot)(slot, idol, method);
	}

	//bool checkMusicDataSatisfy(void* _this, void* unit, int pos = -1) {
	//	static auto musicData_klass = il2cpp_symbols_logged::get_class("PRISM.Legacy.dll", "PRISM.Live", "MusicData");
	//	static auto master_klass = il2cpp_symbols_logged::get_class("PRISM.Definitions.dll", "PRISM.Definitions", "MstSong");
	//
	//	static auto masterData_field = il2cpp_symbols_logged::il2cpp_class_get_field_from_name(musicData_klass, "<Master>k__BackingField");
	//	static auto isSongParts_field = il2cpp_symbols_logged::il2cpp_class_get_field_from_name(master_klass, "<IsSongParts>k__BackingField");
	//
	//	const auto masterData = il2cpp_symbols::read_field(_this, masterData_field);
	//	const auto isSongParts = il2cpp_symbols::read_field<bool>(masterData, isSongParts_field);
	//	if (!isSongParts) return false;
	//
	//	static auto klass_LiveUnit = il2cpp_symbols::get_class_from_instance(unit);
	//	static auto mtd_LiveUnit_get_Idols = il2cpp_class_get_method_from_name(klass_LiveUnit, "get_Idols", 0);
	//	static auto func_LiveUnit_get_Idols = reinterpret_cast<void* (*)(void*, MethodInfo*)>(mtd_LiveUnit_get_Idols->methodPointer);
	//
	//	static auto MusicData_IsOriginalMember = reinterpret_cast<bool (*)(void*, int)>(
	//		il2cpp_symbols_logged::get_method_pointer("PRISM.Legacy.dll", "PRISM.Live", "MusicData", "IsOriginalMember", 1)
	//		);
	//
	//	auto idols = func_LiveUnit_get_Idols(unit, mtd_LiveUnit_get_Idols);
	//	bool ret = true;
	//	int currentSlot = 0;
	//	il2cpp_symbols::iterate_IEnumerable(idols, [&](void* idol) {
	//		__try {
	//			const auto idol_klass = il2cpp_symbols::get_class_from_instance(idol);
	//			const auto characterId_field = il2cpp_class_get_field_from_name(idol_klass, "<CharacterId>k__BackingField");
	//			//const auto characterId = il2cpp_symbols::read_field<int>(idol, characterId_field);
	//			const auto get_CharacterId = reinterpret_cast<int (*)(void*)>(
	//				il2cpp_symbols::get_method_pointer("PRISM.Legacy.dll", "PRISM.Live", "LiveIdol", "get_CharacterId", 0)
	//				);
	//			const auto characterId = get_CharacterId(idol);
	//			const auto isOrigMember = MusicData_IsOriginalMember(_this, characterId);
	//			if (!isOrigMember) {
	//				if (pos == -1) {
	//					ret = false;
	//				}
	//				else {
	//					if (currentSlot == pos) ret = false;
	//				}
	//			}
	//			currentSlot++;
	//		}
	//		__except (seh_filter(GetExceptionInformation())) {
	//			printf("SEH exception detected in `checkMusicDataSatisfy|iterate_IEnumerable`.\n");
	//		}
	//		});
	//
	//	return ret;
	//}
	//
	//HOOK_ORIG_TYPE CheckVocalSeparatedSatisfy_orig;
	//bool CheckVocalSeparatedSatisfy_hook(void* _this, void* unit, void* mtd) {
	//	if (!g_allow_same_idol) {
	//		return HOOK_CAST_CALL(bool, CheckVocalSeparatedSatisfy)(
	//			_this, unit, mtd);
	//	}
	//
	//	return checkMusicDataSatisfy(_this, unit);
	//}
	//
	//HOOK_ORIG_TYPE CheckLimitedVocalSeparatedSatisfy_2_orig;
	//bool CheckLimitedVocalSeparatedSatisfy_2_hook(void* _this, void* unit, int pos, void* mtd) {
	//	if (!g_allow_same_idol) {
	//		return HOOK_CAST_CALL(bool, CheckLimitedVocalSeparatedSatisfy_2)(
	//			_this, unit, pos, mtd);
	//	}
	//	// printf("CheckLimitedVocalSeparatedSatisfy: %d\n", pos);
	//	return checkMusicDataSatisfy(_this, unit, pos);
	//}

	HOOK_ORIG_TYPE LiveMVUnitMemberChangePresenter_initializeAsync_b_4_MoveNext_orig;
	void LiveMVUnitMemberChangePresenter_initializeAsync_b_4_MoveNext_hook(void* _this, MethodInfo* method) {
		slotNewCount = 0;
		settingLiveIdolSlot = true;

		HOOK_CAST_CALL(void, LiveMVUnitMemberChangePresenter_initializeAsync_b_4_MoveNext)(_this, method);
		slotNewCount = 0;
		settingLiveIdolSlot = false;
	}

	HOOK_ORIG_TYPE CriWareErrorHandler_HandleMessage_orig;
	void CriWareErrorHandler_HandleMessage_hook(void* _this, Il2CppString* msg) {
		// wprintf(L"CriWareErrorHandler_HandleMessage: %ls\n%ls\n\n", msg->start_char, environment_get_stacktrace()->start_char);
		environment_get_stacktrace();
		return HOOK_CAST_CALL(void, CriWareErrorHandler_HandleMessage)(_this, msg);
	}

	HOOK_ORIG_TYPE UnsafeLoadBytesFromKey_orig;
	void* UnsafeLoadBytesFromKey_hook(void* _this, Il2CppString* tagName, Il2CppString* assetKey) {
		auto ret = HOOK_CAST_CALL(void*, UnsafeLoadBytesFromKey)(_this, tagName, assetKey);
		// wprintf(L"UnsafeLoadBytesFromKey: tag: %ls, assetKey: %ls\n", tagName->start_char, assetKey->start_char);
		// dumpByteArray(tagName->start_char, assetKey->start_char, ret);
		return ret;
	}

	HOOK_ORIG_TYPE TextLog_AddLog_orig;
	void TextLog_AddLog_hook(void* _this, int speakerFlag, Il2CppString* textID, Il2CppString* text, bool isChoice) {
		return HOOK_CAST_CALL(void, TextLog_AddLog)(_this, speakerFlag, textID, text, isChoice);

		static auto TextLog_klass = il2cpp_symbols::get_class_from_instance(_this);
		static auto dicConvertID_field = il2cpp_class_get_field_from_name(TextLog_klass, "dicConvertID");
		static auto speakerTable_field = il2cpp_class_get_field_from_name(TextLog_klass, "speakerTable");
		static auto listTextLogData_field = il2cpp_class_get_field_from_name(TextLog_klass, "listTextLogData");
		static auto unitIdol_field = il2cpp_class_get_field_from_name(TextLog_klass, "unitIdol");
		static auto convertList_field = il2cpp_class_get_field_from_name(TextLog_klass, "convertList");

		static auto toJsonStr = reinterpret_cast<Il2CppString * (*)(void*)>(
			il2cpp_symbols::get_method_pointer("Newtonsoft.Json.dll", "Newtonsoft.Json",
				"JsonConvert", "SerializeObject", 1)
			);

		auto dicConvertID = il2cpp_field_get_value_object(dicConvertID_field, _this);
		auto speakerTable = il2cpp_field_get_value_object(speakerTable_field, _this);
		auto listTextLogData = il2cpp_field_get_value_object(listTextLogData_field, _this);
		auto unitIdol = il2cpp_field_get_value_object(unitIdol_field, _this);
		auto convertList = il2cpp_field_get_value_object(convertList_field, _this);

		wprintf(L"TextLog_AddLog: speakerFlag: %d, textID: %ls, text: %ls\ndicConvertID:\n%ls\n\nspeakerTable:\n%ls\n\nlistTextLogData:\n%ls\n\nunitIdol:\n%ls\n\nconvertList:\n%ls\n\n",
			speakerFlag, textID->start_char, text->start_char,
			toJsonStr(dicConvertID)->start_char,
			toJsonStr(speakerTable)->start_char,
			toJsonStr(listTextLogData)->start_char,
			toJsonStr(unitIdol)->start_char,
			toJsonStr(convertList)->start_char
		);
	}

	void* baseCameraTransform = nullptr;
	void* baseCamera = nullptr;

	HOOK_ORIG_TYPE Unity_set_fieldOfView_orig;
	void Unity_set_fieldOfView_hook(void* _this, float single) {
		return HOOK_CAST_CALL(void, Unity_set_fieldOfView)(_this, single);
	}
	HOOK_ORIG_TYPE Unity_get_fieldOfView_orig;
	float Unity_get_fieldOfView_hook(void* _this) {
		const auto origFov = HOOK_CAST_CALL(float, Unity_get_fieldOfView)(_this);
		if (_this == baseCamera) {
			if (guiStarting) {
				SCGUIData::sysCamFov = origFov;
			}
			if (g_enable_free_camera) {
				const auto fov = SCCamera::baseCamera.fov;
				Unity_set_fieldOfView_hook(_this, fov);
				return fov;
			}
		}
		// printf("get_fov: %f\n", ret);
		return origFov;
	}

	HOOK_ORIG_TYPE Unity_InternalLookAt_orig;
	void Unity_InternalLookAt_hook(void* _this, Vector3_t worldPosition, Vector3_t worldUp) {
		if (_this == baseCameraTransform) {
			if (g_enable_free_camera) {
				auto pos = SCCamera::baseCamera.getLookAt();
				worldPosition.x = pos.x;
				worldPosition.y = pos.y;
				worldPosition.z = pos.z;
			}
		}
		return HOOK_CAST_CALL(void, Unity_InternalLookAt)(_this, worldPosition, worldUp);
	}

	HOOK_ORIG_TYPE Unity_set_nearClipPlane_orig;
	void Unity_set_nearClipPlane_hook(void* _this, float single) {
		if (_this == baseCamera) {
			if (g_enable_free_camera && g_reenable_clipPlane) {
				single = g_nearClipPlane;
			}
		}
		return HOOK_CAST_CALL(void, Unity_set_nearClipPlane)(_this, single);
	}

	HOOK_ORIG_TYPE Unity_get_nearClipPlane_orig;
	float Unity_get_nearClipPlane_hook(void* _this) {
		auto ret = HOOK_CAST_CALL(float, Unity_get_nearClipPlane)(_this);
		if (_this == baseCamera) {
			if (g_enable_free_camera && g_reenable_clipPlane) {
				ret = g_nearClipPlane;
			}
		}
		return ret;
	}
	HOOK_ORIG_TYPE Unity_get_farClipPlane_orig;
	float Unity_get_farClipPlane_hook(void* _this) {
		auto ret = HOOK_CAST_CALL(float, Unity_get_farClipPlane)(_this);
		if (_this == baseCamera) {
			if (g_enable_free_camera && g_reenable_clipPlane) {
				ret = g_farClipPlane;
			}
		}
		return ret;
	}

	HOOK_ORIG_TYPE Unity_set_farClipPlane_orig;
	void Unity_set_farClipPlane_hook(void* _this, float value) {
		if (_this == baseCamera) {
			if (g_enable_free_camera && g_reenable_clipPlane) {
				value = g_farClipPlane;
			}
		}
		HOOK_CAST_CALL(void, Unity_set_farClipPlane)(_this, value);
	}

	HOOK_ORIG_TYPE Unity_set_rotation_orig;
	void Unity_set_rotation_hook(void* _this, Quaternion_t value) {
		return HOOK_CAST_CALL(void, Unity_set_rotation)(_this, value);
	}
	HOOK_ORIG_TYPE Unity_get_rotation_orig;
	Quaternion_t Unity_get_rotation_hook(void* _this) {
		auto ret = HOOK_CAST_CALL(Quaternion_t, Unity_get_rotation)(_this);

		if (_this == baseCameraTransform) {
			if (guiStarting) {
				SCGUIData::sysCamRot.w = ret.w;
				SCGUIData::sysCamRot.x = ret.x;
				SCGUIData::sysCamRot.y = ret.y;
				SCGUIData::sysCamRot.z = ret.z;
				SCGUIData::updateSysCamLookAt();
			}
			if (g_enable_free_camera) {
				ret.w = 0;
				ret.x = 0;
				ret.y = 0;
				ret.z = 0;
				Unity_set_rotation_hook(_this, ret);

				static auto Vector3_klass = il2cpp_symbols::get_class("UnityEngine.CoreModule.dll", "UnityEngine", "Vector3");
				Vector3_t* pos = reinterpret_cast<Vector3_t*>(il2cpp_object_new(Vector3_klass));
				Vector3_t* up = reinterpret_cast<Vector3_t*>(il2cpp_object_new(Vector3_klass));
				up->x = 0;
				up->y = 1;
				up->z = 0;
				Unity_InternalLookAt_hook(_this, *pos, *up);
			}
		}

		return ret;
	}

	HOOK_ORIG_TYPE Unity_set_position_orig;
	void Unity_set_position_hook(void* _this, Vector3_t value) {
		return HOOK_CAST_CALL(void, Unity_set_position)(_this, value);
	}

	HOOK_ORIG_TYPE Unity_get_position_orig;
	Vector3_t Unity_get_position_hook(void* _this) {
		auto data = HOOK_CAST_CALL(Vector3_t, Unity_get_position)(_this);
		if (_this == baseCameraTransform) {
			auto ret = Unity_get_rotation_hook(_this);
			if (guiStarting) {
				SCGUIData::sysCamPos.x = data.x;
				SCGUIData::sysCamPos.y = data.y;
				SCGUIData::sysCamPos.z = data.z;

				SCGUIData::sysCamRot.w = ret.w;
				SCGUIData::sysCamRot.x = ret.x;
				SCGUIData::sysCamRot.y = ret.y;
				SCGUIData::sysCamRot.z = ret.z;
				SCGUIData::updateSysCamLookAt();
			}
			if (g_enable_free_camera) {
				SCCamera::baseCamera.updateOtherPos(&data);
				Unity_set_position_hook(_this, data);

				ret.w = 0;
				ret.x = 0;
				ret.y = 0;
				ret.z = 0;
				Unity_set_rotation_hook(_this, ret);

				static auto Vector3_klass = il2cpp_symbols::get_class("UnityEngine.CoreModule.dll", "UnityEngine", "Vector3");
				Vector3_t* pos = reinterpret_cast<Vector3_t*>(il2cpp_object_new(Vector3_klass));
				Vector3_t* up = reinterpret_cast<Vector3_t*>(il2cpp_object_new(Vector3_klass));
				up->x = 0;
				up->y = 1;
				up->z = 0;
				Unity_InternalLookAt_hook(_this, *pos, *up);
			}
		}

		return data;
	}


	HOOK_ORIG_TYPE get_baseCamera_orig;
	void* get_baseCamera_hook(void* _this) {
		// printf("get_baseCamera\n");
		auto ret = HOOK_CAST_CALL(void*, get_baseCamera)(_this);  // UnityEngine.Camera

		// always save the camera as users may enable the gui option after the init
		static auto GetComponent = reinterpret_cast<void* (*)(void*, Il2CppReflectionType*)>(
			il2cpp_symbols::get_method_pointer("UnityEngine.CoreModule.dll", "UnityEngine",
				"Component", "GetComponent", 1));

		static auto transClass = il2cpp_symbols::get_class("UnityEngine.CoreModule.dll", "UnityEngine", "Transform");
		static auto transType = il2cpp_type_get_object(il2cpp_class_get_type(transClass));

		auto transform = GetComponent(ret, transType);  // UnityEngine.Transform
		if (transform) {
			baseCameraTransform = transform;
			baseCamera = ret;
		}

		return ret;
	}

	uintptr_t GetSubject_OnNext_addr() {
		// var managedGenericArguments = [typeof(PRISM.Adapters.CostumeChange.CostumeChangeViewModel)];
		auto refltype_CostumeChangeViewModel = reflection::typeof("PRISM.Adapters.dll", "PRISM.Adapters.CostumeChange", "CostumeChangeViewModel");
		auto managedGenericArguments = reflection::CreateManagedTypeArray({ refltype_CostumeChangeViewModel });

		// var refltype_Subject_closed = RuntimeType.MakeGenericType(typeof(UniRx.Subject<>), managedGenericArguments);
		auto method_RuntimeType_MakeGenericType_2 = il2cpp_symbols_logged::get_method_corlib("System", "RuntimeType", "MakeGenericType", 2);
		auto refltype_Subject_T = reflection::typeof("UniRx.dll", "UniRx", "Subject`1");
		Il2CppObject* args_RuntimeType_MakeGenericType_2[2]{ refltype_Subject_T, managedGenericArguments };
		auto refltype_Subject_closed = (Il2CppReflectionType*)reflection::Invoke(method_RuntimeType_MakeGenericType_2, nullptr, args_RuntimeType_MakeGenericType_2, "RuntimeType::MakeGenericType(2)");

		// var method_Subject_OnNext = refltype_Subject_closed.GetMethod("OnNext");
		auto method_Subject_OnNext = il2cpp_class_get_method_from_name(il2cpp_class_from_system_type(refltype_Subject_closed), "OnNext", 1);

		return method_Subject_OnNext->methodPointer;
	}

	void ReadPreviewCostumeStatus(Il2CppObject* viewModel, UnitIdol& unitIdol) {
		static auto method_get_PreviewCostumeSet = il2cpp_symbols_logged::get_method(
			"PRISM.Adapters.dll", "PRISM.Adapters.CostumeChange",
			"CostumeChangeViewModel", "get_PreviewCostumeSet", 0
		);
		static auto method_get_Costume = il2cpp_symbols_logged::get_method(
			"PRISM.Legacy.dll", "PRISM.Domain",
			"CostumeSetData", "get_Costume", 0
		);

		static auto klass_CostumeStatus = il2cpp_symbols_logged::get_class(
			"PRISM.Module.Networking.dll", "PRISM.Module.Networking.Stub.Status", "CostumeStatus"
		);
		static auto method_get_MstCostumeId = il2cpp_symbols_logged::get_method(klass_CostumeStatus, "get_MstCostumeId", 0);
		static auto method_get_MstCharacterInfoId = il2cpp_symbols_logged::get_method(klass_CostumeStatus, "get_MstCharacterInfoId", 0);
		static auto method_get_CostumeType = il2cpp_symbols_logged::get_method(klass_CostumeStatus, "get_CostumeType", 0);
		static auto method_get_ResourceId = il2cpp_symbols_logged::get_method(klass_CostumeStatus, "get_ResourceId", 0);

		auto previewCostumeSet = method_get_PreviewCostumeSet->Invoke(viewModel, {});
		auto costume = method_get_Costume->Invoke(previewCostumeSet, {});

		unitIdol.CostumeStatusLoaded = true;
		unitIdol.CostumeMstCostumeId = method_get_MstCostumeId->Invoke(costume, {})->unbox_value<int>();
		unitIdol.CostumeMstCharacterInfoId = method_get_MstCharacterInfoId->Invoke(costume, {})->unbox_value<int>();
		unitIdol.CostumeType = method_get_CostumeType->Invoke(costume, {})->unbox_value<int>();
		unitIdol.CostumeResourceId = method_get_ResourceId->Invoke(costume, {})->unbox_value<int>();
	}

	void ReplaceAutoAppliedCostume(managed::UnitIdol* idol) {
		auto it = autoAppliedCostumes.find(UnitIdol::GetCharaId(idol));
		if (it != autoAppliedCostumes.end())
			it->second.ApplyTo(idol, false);
	}

	HOOK_DEF(void, AssembleCharacter_AssembleFileCache)(void* downloadKeyContainer, managed::UnitIdol* idol) {
		if (g_apply_costumes_automatically) ReplaceAutoAppliedCostume(idol);
		HOOK_CAST_CALL(void, AssembleCharacter_AssembleFileCache)(downloadKeyContainer, idol);
	}

	HOOK_DEF(void*, SpawnCharacter_CreateCharacter)(void* _this, managed::UnitIdol* idol) {
		if (g_apply_costumes_automatically) ReplaceAutoAppliedCostume(idol);
		return HOOK_CAST_CALL(void*, SpawnCharacter_CreateCharacter)(_this, idol);
	}

	void (*fp_CostumeChangeViewModel_Apply)(void* _this);

	HOOK_ORIG_TYPE Subject_OnNext_orig;
	void Subject_OnNext_hook(void* _this, void* value, void* mi) {
		static auto method_GetPreviewUnitIdol = il2cpp_symbols_logged::get_method(
			"PRISM.Adapters.dll", "PRISM.Adapters.CostumeChange",
			"CostumeChangeViewModel", "GetPreviewUnitIdol", 0
		);

		auto klass = value != nullptr ? il2cpp_object_get_class((Il2CppObject*)value) : nullptr;
		const bool isCostumeChangeViewModel = klass != nullptr &&
			0 == strcmp("CostumeChangeViewModel", il2cpp_class_get_name(klass));

		if (isCostumeChangeViewModel && g_apply_costumes_automatically)
		{
			fp_CostumeChangeViewModel_Apply(value);

			auto idol = method_GetPreviewUnitIdol->Invoke<managed::UnitIdol*>(((Il2CppObject*)value), {});
			int charaId = UnitIdol::GetCharaId(idol);
			if (charaId >= 0)
				autoAppliedCostumes[charaId].ReadFrom(idol);
		}

		HOOK_CAST_CALL(void, Subject_OnNext)(_this, value, mi);

		if (!isCostumeChangeViewModel) return;

		static MethodInfo* mtd_CostumeChangeViewModel_GetPreviewUnitIdol;
		static managed::UnitIdol* (*func_CostumeChangeViewModel_GetPreviewUnitIdol)(void* _this, void* mtd);

		if (g_save_and_replace_costume_changes) {
			if (mtd_CostumeChangeViewModel_GetPreviewUnitIdol == nullptr) {
				mtd_CostumeChangeViewModel_GetPreviewUnitIdol = il2cpp_class_get_method_from_name(klass, "GetPreviewUnitIdol", 0);
				func_CostumeChangeViewModel_GetPreviewUnitIdol = reinterpret_cast<managed::UnitIdol * (*)(void* _this, void* mtd)>(mtd_CostumeChangeViewModel_GetPreviewUnitIdol->methodPointer);
			}

			auto idol = func_CostumeChangeViewModel_GetPreviewUnitIdol(value, mtd_CostumeChangeViewModel_GetPreviewUnitIdol);

			UnitIdol data;
			data.ReadFrom(idol);
			// `GetPreviewUnitIdol` only returns a base UnitIdol without MstCostume
			// an extra call to fill `MstCostumeId` is required
			ReadPreviewCostumeStatus((Il2CppObject*)value, data);

			std::cout << "Saved UnitIdel = ";
			data.Print(std::cout);

			if (data.CharaId >= 0)
				savedCostumes[data.CharaId] = data;

			lastSavedCostume = data;
		}
	}


#define MAGICACLOTH_GET_CLASS(_str_name_) \
	((Il2CppClass*)il2cpp_symbols_logged::get_class("MagicaClothV2.dll", "MagicaCloth2", _str_name_))
#define MAGICACLOTH_SET_INERTIA_LIMIT(_txt_field_name_, _val_value_) \
	static auto field_InertiaConstraint_SerializeData_##_txt_field_name_ = il2cpp_class_get_field_from_name(klass_InertiaConstraint_SerializeData, #_txt_field_name_); \
	auto _txt_field_name_ = (managed::MagicaCloth2::CheckSliderSerializeData*)il2cpp_field_get_value_object(field_InertiaConstraint_SerializeData_##_txt_field_name_, inertiaConstraint); \
	_txt_field_name_->value = _val_value_;

	void ModifyMagicaCloth(Il2CppObject* cloth) {
		if (g_magicacloth_output_cloth) {
			auto name = il2cpp_symbols::get_unity_gameobject_fullname(cloth);
			std::cout << "ModifyMagicaCloth: " << name << std::endl;
		}

		if (!g_magicacloth_override) return;

		static auto klass_MagicaCloth = MAGICACLOTH_GET_CLASS("MagicaCloth");
		static auto klass_ClothSerializeData = MAGICACLOTH_GET_CLASS("ClothSerializeData");
		static auto klass_InertiaConstraint = MAGICACLOTH_GET_CLASS("InertiaConstraint");
		static auto klass_InertiaConstraint_SerializeData = klass_InertiaConstraint->GetNestedClass("SerializeData");
		static auto klass_AngleConstraint = MAGICACLOTH_GET_CLASS("AngleConstraint");
		static auto klass_AngleConstraint_LimitSerializeData = il2cpp_symbols::get_nested_class(klass_AngleConstraint, "LimitSerializeData");

		static auto field_MagicaCloth_serializeData = klass_MagicaCloth->GetField("serializeData");
		auto sdata = field_MagicaCloth_serializeData->GetValueObject(cloth);

		static auto field_ClothSerializeData_damping = klass_ClothSerializeData->GetField("damping");

		auto damping = field_ClothSerializeData_damping->GetValueObject<managed::MagicaCloth2::CurveSerializeData*>(sdata);
		damping->value = g_magicacloth_damping;

		static auto field_ClothSerializeData_inertiaConstraint = klass_ClothSerializeData->GetField("inertiaConstraint");
		auto inertiaConstraint = field_ClothSerializeData_inertiaConstraint->GetValueObject<managed::MagicaCloth2::InertiaConstraintSerializeData*>(sdata);
		MAGICACLOTH_SET_INERTIA_LIMIT(movementSpeedLimit, g_magicacloth_movementSpeedLimit);
		MAGICACLOTH_SET_INERTIA_LIMIT(rotationSpeedLimit, g_magicacloth_rotationSpeedLimit);
		MAGICACLOTH_SET_INERTIA_LIMIT(localMovementSpeedLimit, g_magicacloth_localMovementSpeedLimit);
		MAGICACLOTH_SET_INERTIA_LIMIT(localRotationSpeedLimit, g_magicacloth_localRotationSpeedLimit);
		MAGICACLOTH_SET_INERTIA_LIMIT(particleSpeedLimit, g_magicacloth_particleSpeedLimit);

		static auto field_ClothSerializeData_angleLimitConstraint = klass_ClothSerializeData->GetField("angleLimitConstraint");
		auto angleLimitConstraint = field_ClothSerializeData_angleLimitConstraint->GetValueObject(sdata);
		static auto field_AngleConstraint_LimitSerializeData_limitAngle = klass_AngleConstraint_LimitSerializeData->GetField("limitAngle");
		auto limitAngle = field_AngleConstraint_LimitSerializeData_limitAngle->GetValueObject<managed::MagicaCloth2::CurveSerializeData*>(angleLimitConstraint);
		limitAngle->value = g_magicacloth_limitAngle;

		static auto field_ClothSerializeData_springConstraint = klass_ClothSerializeData->GetField("springConstraint");
		auto springConstraint = field_ClothSerializeData_springConstraint->GetValueObject<managed::MagicaCloth2::SpringConstraintSerializeData*>(sdata);
		springConstraint->limitDistance = g_magicacloth_springLimitDistance;
		springConstraint->springNoise = g_magicacloth_springNoise;
	}

	HOOK_ORIG_TYPE MagicaCloth_BuildAndRun_orig;
	bool MagicaCloth_BuildAndRun_hook(void* _this) {
		ModifyMagicaCloth((Il2CppObject*)_this);
		return HOOK_CAST_CALL(bool, MagicaCloth_BuildAndRun)(_this);
	}

	HOOK_ORIG_TYPE MagicaCloth_SetParameterChange_orig;
	void MagicaCloth_SetParameterChange_hook(void* _this) {
		ModifyMagicaCloth((Il2CppObject*)_this);
		HOOK_CAST_CALL(void, MagicaCloth_SetParameterChange)(_this);
	}

	HOOK_ORIG_TYPE MagicaClothController_get_Inertia_orig;
	void* MagicaClothController_get_Inertia_hook(void* _this, MethodInfo* mi) {
		auto value = (managed::MagicaCloth2::BodyParamFloatProperty*)HOOK_CAST_CALL(void*, MagicaClothController_get_Inertia)(_this, mi);
		if (mi->IsName("get_Inertia")) {
			if (g_magicacloth_output_controller) {
				printf("MagicaClothController_get_Inertia_hook\n");
			}
			if (g_magicacloth_override) {
				value->MinValue = g_magicacloth_inertia_min;
				value->MaxValue = g_magicacloth_inertia_max;
			}
		}
		return value;
	}

	HOOK_ORIG_TYPE MagicaClothController_get_Radius_orig;
	void* MagicaClothController_get_Radius_hook(void* _this, MethodInfo* mi) {
		auto value = (managed::MagicaCloth2::BodyParamFloatProperty*)HOOK_CAST_CALL(void*, MagicaClothController_get_Radius)(_this, mi);
		if (mi->IsName("get_Radius")) {
			if (g_magicacloth_output_controller) {
				printf("MagicaClothController_get_Radius_hook\n");
			}
			if (g_magicacloth_override) {
				value->MinValue = g_magicacloth_radius_min;
				value->MaxValue = g_magicacloth_radius_max;
			}
		}
		return value;
	}

	HOOK_ORIG_TYPE MagicaClothController_Awake_orig;
	void MagicaClothController_Awake_hook(Il2CppObject* _this) {
		HOOK_CAST_CALL(void, MagicaClothController_Awake)(_this);

		if (g_magicacloth_output_controller) {
			printf("MagicaClothController_Awake_hook\n");
		}

		if (g_magicacloth_override) {
			static auto klass = il2cpp_symbols_logged::get_class("PRISM.Module.CustomMagicaCloth.dll", "PRISM.Module.CustomMagicaCloth", "MagicaClothController");
			static auto method_get_Inertia = il2cpp_class_get_method_from_name(klass, "get_Inertia", 0);
			auto method_get_Radius = il2cpp_class_get_method_from_name(klass, "get_Radius", 0);

			auto inertia = method_get_Inertia->Invoke<managed::MagicaCloth2::BodyParamFloatProperty*>(_this, {});
			inertia->MinValue = g_magicacloth_inertia_min;
			inertia->MaxValue = g_magicacloth_inertia_max;

			auto radius = method_get_Radius->Invoke<managed::MagicaCloth2::BodyParamFloatProperty*>(_this, {});
			radius->MinValue = g_magicacloth_radius_min;
			radius->MaxValue = g_magicacloth_radius_max;
		}
	}


	HOOK_DEF(void, CostumeChangeViewModel__ctor)(void* _this, void* parameter, int characterId, void* settingCostumeSet, bool isAllDressOrdered, bool isEnableDressOrderTab, void* defaultCostumeSet) {
		if (g_show_hidden_costumes) {
			isAllDressOrdered = true;
		}
		HOOK_CAST_CALL(void, CostumeChangeViewModel__ctor)(_this, parameter, characterId, settingCostumeSet, isAllDressOrdered, isEnableDressOrderTab, defaultCostumeSet);
	}

	// [PATCH] v20 - the footer's "決定 / 解放" button is driven by these checks.
	// With every costume unlocked in the data, the only false results come from
	// accessory parts whose Status was built as Unlockable/Locked, so we let
	// the decide button win there (and log what happens for the next round).
	HOOK_DEF(bool, CostumeChangeViewModel_CanDecide0)(void* _this) {
		if (g_apply_costumes_automatically) {
			return false;
		}
		bool ret = HOOK_CAST_CALL(bool, CostumeChangeViewModel_CanDecide0)(_this);
		if (!ret || UnlockDiag::g_decide0Logs < 60) {
			if (UnlockDiag::g_decide0Logs < 200) {
				UnlockDiag::g_decide0Logs++;
				ViewDiag::appendLog("unlock-diag.txt", std::format("candecide0 ret={}{}", ret ? 1 : 0, ret ? "" : "->1"));
			}
		}
		return true;
	}

	HOOK_DEF(bool, CostumeChangeViewModel_CanDecide2)(void* _this, int partType, bool isAllDressOrdered) {
		if (g_apply_costumes_automatically) {
			return false;
		}
		bool ret = HOOK_CAST_CALL(bool, CostumeChangeViewModel_CanDecide2)(_this, partType, isAllDressOrdered);
		const bool accType = partType >= 2 && partType <= 7;
		if (!ret || (accType && UnlockDiag::g_decide2Logs < 60)) {
			if (UnlockDiag::g_decide2Logs < 200) {
				UnlockDiag::g_decide2Logs++;
				ViewDiag::appendLog("unlock-diag.txt", std::format(
					"candecide2 type={} allDress={} ret={}{}",
					partType, isAllDressOrdered ? 1 : 0, ret ? 1 : 0, ret ? "" : "->1"));
			}
		}
		if (!ret && accType) ret = true;
		return ret;
	}

	// [DIAG] v20 - which flag the footer actually flips on selection changes.
	HOOK_DEF(void, CostumeChangeFooterView_SetShowDecide)(void* _this, bool isShow) {
		if (UnlockDiag::g_footLogs < 300) {
			UnlockDiag::g_footLogs++;
			ViewDiag::appendLog("unlock-diag.txt", std::format("footer isShow={}", isShow ? 1 : 0));
		}
		HOOK_CAST_CALL(void, CostumeChangeFooterView_SetShowDecide)(_this, isShow);
	}

	HOOK_DEF(void, CostumeChangeViewModel_RefreshViewModels)(void* _this) {

		auto method_GetPreviewUnitIdol = il2cpp_symbols_logged::get_method(
			"PRISM.Adapters.dll", "PRISM.Adapters.CostumeChange",
			"CostumeChangeViewModel", "GetPreviewUnitIdol", 0
		);
		auto previewUnitIdol = (managed::UnitIdol*)method_GetPreviewUnitIdol->Invoke((Il2CppObject*)_this, {});
		auto it = savedCostumes.find(UnitIdol::GetCharaId(previewUnitIdol));
		if (it != savedCostumes.end()) {
			it->second.ApplyTo(previewUnitIdol, false);
			std::cout << "CharaId " << it->first << " has been modified." << std::endl;
		}

		HOOK_CAST_CALL(void, CostumeChangeViewModel_RefreshViewModels)(_this);
	}


	void readDMMGameGuardData();

	HOOK_ORIG_TYPE GGIregualDetector_ShowPopup_orig;
	void GGIregualDetector_ShowPopup_hook(void* _this, int code) {
		static auto End = reinterpret_cast<void (*)(void*)>(
			il2cpp_symbols::get_method_pointer(
				"PRISM.Legacy.dll", "PRISM",
				"GGIregualDetector", "End", 0
			));

		//printf("GGIregualDetector_ShowPopup: code: %d\n%ls\n\n", code, environment_get_stacktrace()->start_char);
		//readDMMGameGuardData();

		return End(_this);

		static auto this_klass = il2cpp_symbols::get_class_from_instance(_this);
		static FieldInfo* isShowPopup_field = il2cpp_class_get_field_from_name(this_klass, "_isShowPopup");
		static FieldInfo* isError_field = il2cpp_class_get_field_from_name(this_klass, "_isError");
		auto fieldData = il2cpp_class_get_static_field_data(this_klass);
		auto isError = il2cpp_symbols::read_field<bool>(_this, isError_field);

		if (isError) {
			// if (code == 0) return;
		}

		return HOOK_CAST_CALL(void, GGIregualDetector_ShowPopup)(_this, code);
	}

	HOOK_ORIG_TYPE DMMGameGuard_NPGameMonCallback_orig;
	int DMMGameGuard_NPGameMonCallback_hook(UINT dwMsg, UINT dwArg) {
		// printf("DMMGameGuard_NPGameMonCallback: dwMsg: %u, dwArg: %u\n%ls\n\n", dwMsg, dwArg, environment_get_stacktrace()->start_char);
		return 1;
		// return HOOK_CAST_CALL(void, DMMGameGuard_NPGameMonCallback)(dwMsg, dwArg);
	}

	HOOK_ORIG_TYPE set_vsync_count_orig;
	void set_vsync_count_hook(int value) {
		return HOOK_CAST_CALL(void, set_vsync_count)(g_vsync_count == -1 ? value : g_vsync_count);
	}

	HOOK_ORIG_TYPE set_fps_orig;
	void set_fps_hook(int value) {
		return HOOK_CAST_CALL(void, set_fps)(g_max_fps == -1 ? value : g_max_fps);
	}

	HOOK_ORIG_TYPE Unity_Quit_orig;
	void Unity_Quit_hook(int code) {
		printf("Quit code: %d\n", code);
		TerminateProcess(GetCurrentProcess(), 0);
		// printf("Quit code: %d\n%ls\n\n", code, environment_get_stacktrace()->start_char);
		// return HOOK_CAST_CALL(void, Unity_Quit)(code);
	}

	HOOK_ORIG_TYPE SetCallbackToGameMon_orig;
	int SetCallbackToGameMon_hook(void* DMMGameGuard_NPGameMonCallback_addr) {
		// printf("SetCallbackToGameMon: %p\n%ls\n\n", DMMGameGuard_NPGameMonCallback_addr, environment_get_stacktrace()->start_char);
		ADD_HOOK(DMMGameGuard_NPGameMonCallback, "DMMGameGuard_NPGameMonCallback at %p");
		return HOOK_CAST_CALL(int, SetCallbackToGameMon)(DMMGameGuard_NPGameMonCallback_addr);
	}

	HOOK_ORIG_TYPE DMMGameGuard_Setup_orig;
	void DMMGameGuard_Setup_hook() {
		// printf("\n\nDMMGameGuard_Setup\n\n");
		readDMMGameGuardData();
	}

	HOOK_ORIG_TYPE DMMGameGuard_SetCheckMode_orig;
	void DMMGameGuard_SetCheckMode_hook(bool isCheck) {
		// printf("DMMGameGuard_SetCheckMode before\n");
		// readDMMGameGuardData();
		HOOK_CAST_CALL(void, DMMGameGuard_SetCheckMode)(isCheck);
		// printf("DMMGameGuard_SetCheckMode after\n");
		// readDMMGameGuardData();
	}

	HOOK_ORIG_TYPE PreInitNPGameMonW_orig;
	void PreInitNPGameMonW_hook(int64_t v) {
		// printf("PreInitNPGameMonW\n");
		// readDMMGameGuardData();
	}
	HOOK_ORIG_TYPE PreInitNPGameMonA_orig;
	void PreInitNPGameMonA_hook(int64_t v) {
		// printf("PreInitNPGameMonA\n");
		// readDMMGameGuardData();
	}

	HOOK_ORIG_TYPE InitNPGameMon_orig;
	UINT InitNPGameMon_hook() {
		// printf("InitNPGameMon\n");
		// readDMMGameGuardData();
		return 1877;
	}

	HOOK_ORIG_TYPE SetHwndToGameMon_orig;
	void SetHwndToGameMon_hook(int64_t v) {
		// printf("SetHwndToGameMon\n");
		// readDMMGameGuardData();
	}

	void* DMMGameGuard_klass;
	FieldInfo* isCheck_field;
	FieldInfo* isInit_field;
	FieldInfo* bAppExit_field;
	FieldInfo* errCode_field;

	void readDMMGameGuardData() {
		if (!DMMGameGuard_klass) return;
		if (!isCheck_field) {
			printf("isCheck_field invalid: %p\n", isCheck_field);
			return;
		}

		auto staticData = il2cpp_class_get_static_field_data(DMMGameGuard_klass);
		auto isCheck = il2cpp_symbols::read_field<bool>(staticData, isCheck_field);
		auto isInit = il2cpp_symbols::read_field<bool>(staticData, isInit_field);
		auto bAppExit = il2cpp_symbols::read_field<bool>(staticData, bAppExit_field);
		auto errCode = il2cpp_symbols::read_field<int>(staticData, errCode_field);
		if (bAppExit) {
			il2cpp_symbols::write_field(staticData, bAppExit_field, false);
		}

		printf("DMMGameGuardData: isCheck: %d, isInit: %d, bAppExit: %d, errCode: %d\n\n", isCheck, isInit, bAppExit, errCode);
	}

	bool pathed = false;
	bool npPatched = false;

	void patchNP(HMODULE module) {
		if (npPatched) return;
		npPatched = true;
		// auto np_module = GetModuleHandle("NPGameDLL");
		if (module) {
			auto SetCallbackToGameMon_addr = GetProcAddress(module, "SetCallbackToGameMon");
			auto PreInitNPGameMonW_addr = GetProcAddress(module, "PreInitNPGameMonW");
			auto PreInitNPGameMonA_addr = GetProcAddress(module, "PreInitNPGameMonA");
			auto InitNPGameMon_addr = GetProcAddress(module, "InitNPGameMon");
			auto SetHwndToGameMon_addr = GetProcAddress(module, "SetHwndToGameMon");

			ADD_HOOK(SetCallbackToGameMon, "SetCallbackToGameMon ap %p");
			ADD_HOOK(PreInitNPGameMonW, "PreInitNPGameMonW ap %p");
			ADD_HOOK(PreInitNPGameMonA, "PreInitNPGameMonA ap %p");
			ADD_HOOK(InitNPGameMon, "InitNPGameMon ap %p");
			ADD_HOOK(SetHwndToGameMon, "SetHwndToGameMon ap %p");
		}
		else {
			printf("NPGameDLL Get failed.\n");
		}
	}

	int retryCount = 0;
	void path_game_assembly()
	{
		if (!mh_inited)
			return;

		auto il2cpp_module = GetModuleHandle("GameAssembly.dll");
		if (!il2cpp_module) {
			printf("GameAssembly.dll not loaded.\n");
			retryCount++;
			if (retryCount == 15) {
				reopen_self();
				return;
			}
		}

		if (pathed) return;
		pathed = true;

		printf("Trying to patch GameAssembly.dll...\n");
		initcharaSwayStringOffset();

		// load il2cpp exported functions
		il2cpp_symbols::init(il2cpp_module);

#pragma region HOOK_ADDRESSES

		environment_get_stacktrace = reinterpret_cast<decltype(environment_get_stacktrace)>(
			il2cpp_symbols::get_method_pointer("mscorlib.dll", "System",
				"Environment", "get_StackTrace", 0)
			);

		DMMGameGuard_klass = il2cpp_symbols::get_class("PRISM.Legacy.dll", "PRISM", "DMMGameGuard");
		isCheck_field = il2cpp_class_get_field_from_name(DMMGameGuard_klass, "_isCheck");
		isInit_field = il2cpp_class_get_field_from_name(DMMGameGuard_klass, "_isInit");
		bAppExit_field = il2cpp_class_get_field_from_name(DMMGameGuard_klass, "_bAppExit");
		errCode_field = il2cpp_class_get_field_from_name(DMMGameGuard_klass, "_errCode");
		initEncoding();
		ReadAllBytes = reinterpret_cast<decltype(ReadAllBytes)>(
			il2cpp_symbols::get_method_pointer("mscorlib.dll", "System.IO", "File", "ReadAllBytes", 1)
			);

		AssetBundle_LoadFromFile = reinterpret_cast<decltype(AssetBundle_LoadFromFile)>(
			il2cpp_symbols::get_method_pointer(
				"UnityEngine.AssetBundleModule.dll", "UnityEngine",
				"AssetBundle", "LoadFromFile", 3
			)
			);
		Object_IsNativeObjectAlive = reinterpret_cast<bool(*)(void*)>(
			il2cpp_symbols::get_method_pointer("UnityEngine.CoreModule.dll", "UnityEngine",
				"Object", "IsNativeObjectAlive", 1)
			);
		AssetBundle_LoadAsset = reinterpret_cast<decltype(AssetBundle_LoadAsset)>(
			il2cpp_symbols_logged::get_method_pointer("UnityEngine.AssetBundleModule.dll", "UnityEngine", "AssetBundle", "LoadAsset_Internal", 2)
			);
		const auto FontClass = il2cpp_symbols::get_class("UnityEngine.TextRenderingModule.dll", "UnityEngine", "Font");
		Font_Type = il2cpp_type_get_object(il2cpp_class_get_type(FontClass));
		TMP_FontAsset_CreateFontAsset = reinterpret_cast<decltype(TMP_FontAsset_CreateFontAsset)>(il2cpp_symbols::get_method_pointer(
			"Unity.TextMeshPro.dll", "TMPro",
			"TMP_FontAsset", "CreateFontAsset", 1
		));
		TMP_Text_set_font = reinterpret_cast<decltype(TMP_Text_set_font)>(il2cpp_symbols::get_method_pointer(
			"Unity.TextMeshPro.dll", "TMPro",
			"TMP_Text", "set_font", 1
		));
		TMP_Text_get_font = reinterpret_cast<decltype(TMP_Text_get_font)>(il2cpp_symbols::get_method_pointer(
			"Unity.TextMeshPro.dll", "TMPro",
			"TMP_Text", "get_font", 0
		));

		const auto TMP_Text_set_text_addr = il2cpp_symbols::get_method_pointer(
			"Unity.TextMeshPro.dll", "TMPro",
			"TMP_Text", "set_text", 1
		);
		const auto UITextMeshProUGUI_Awake_addr = il2cpp_symbols::get_method_pointer(
			"PRISM.Legacy.dll", "ENTERPRISE.UI",
			"UITextMeshProUGUI", "Awake", 0
		);

		auto ScenarioManager_Init_addr = il2cpp_symbols::get_method_pointer(
			"PRISM.Legacy.dll", "PRISM.Scenario",
			"ScenarioManager", "_initializeAsync", 1
		);
		auto DataFile_GetBytes_addr = il2cpp_symbols::get_method_pointer(
			"PRISM.Legacy.dll", "PRISM",
			"DataFile", "GetBytes", 1
		);
		DataFile_IsKeyExist = reinterpret_cast<decltype(DataFile_IsKeyExist)>(
			il2cpp_symbols::get_method_pointer(
				"PRISM.Legacy.dll", "PRISM",
				"DataFile", "IsKeyExist", 1
			)
			);

		auto SetResolution_addr = il2cpp_symbols::get_method_pointer(
			"UnityEngine.CoreModule.dll", "UnityEngine",
			"Screen", "SetResolution", 3
		);

		//auto UnsafeLoadBytesFromKey_addr = il2cpp_symbols::get_method_pointer(
		//	"PRISM.ResourceManagement.dll", "PRISM.ResourceManagement",
		//	"ResourceLoader", "UnsafeLoadBytesFromKey", 2
		//);

		auto TextLog_AddLog_addr = il2cpp_symbols::get_method_pointer(
			"PRISM.Legacy.dll", "PRISM.Scenario",
			"TextLog", "AddLog", 4
		);

		auto StoryExtensions_IsLocked_addr = il2cpp_symbols::get_method_pointer(
			"PRISM.Legacy.dll", "PRISM.Domain",
			"StoryExtensions", "IsLocked", 1
		);
		auto DioramaProductViewModel_IsLocked_addr = il2cpp_symbols_logged::get_method_pointer(
			"PRISM.Adapters.dll", "PRISM.Adapters",
			"DioramaProductViewModel", "IsLocked", 0
		);

		auto RunwayPoseSettingContentViewModel_ctor_addr = il2cpp_symbols_logged::get_method_pointer(
			"PRISM.Adapters.dll", "PRISM.Adapters",
			"RunwayPoseSettingContentViewModel", ".ctor", 2
		);
		auto LocalizationManager_GetTextOrNull_addr = il2cpp_symbols::get_method_pointer(
			"PRISM.Legacy.dll", "ENTERPRISE.Localization",
			"LocalizationManager", "GetTextOrNull", 2
		);
		auto GetResolutionSize_addr = il2cpp_symbols::get_method_pointer(
			"Prism.Rendering.Runtime.dll", "PRISM.Rendering",
			"RenderManager", "GetResolutionSize", 1
		);
		auto LiveMVOverlayView_UpdateLyrics_addr = il2cpp_symbols::get_method_pointer(
			"PRISM.Interactions.Live.dll", "PRISM.Interactions.Live",
			"LiveMVOverlayView", "UpdateLyrics", 1
		);
		auto TimelineController_SetLyric_addr = il2cpp_symbols::get_method_pointer(
			"PRISM.Legacy.dll", "PRISM",
			"TimelineController", "SetLyric", 1
		);

		auto get_baseCamera_addr = il2cpp_symbols::get_method_pointer(
			"PRISM.Legacy.dll", "PRISM",
			"CameraController", "get_BaseCamera", 0
		);

		const auto AssetBundle_LoadAsset_addr = il2cpp_symbols_logged::get_method_pointer("UnityEngine.AssetBundleModule.dll", "UnityEngine", "AssetBundle", "LoadAsset_Internal", 2);

		auto Unity_get_position_addr = il2cpp_symbols_logged::get_method_pointer("UnityEngine.CoreModule.dll", "UnityEngine", "Transform", "get_position", 0);
		auto Unity_set_position_addr = il2cpp_symbols_logged::get_method_pointer("UnityEngine.CoreModule.dll", "UnityEngine", "Transform", "set_position", 1);

		auto Unity_get_fieldOfView_addr = il2cpp_symbols_logged::get_method_pointer("UnityEngine.CoreModule.dll", "UnityEngine", "Camera", "get_fieldOfView", 0);
		auto Unity_set_fieldOfView_addr = il2cpp_symbols_logged::get_method_pointer("UnityEngine.CoreModule.dll", "UnityEngine", "Camera", "set_fieldOfView", 1);
		auto Unity_InternalLookAt_addr = il2cpp_symbols_logged::get_method_pointer("UnityEngine.CoreModule.dll", "UnityEngine", "Transform", "Internal_LookAt", 2);
		auto Unity_set_nearClipPlane_addr = il2cpp_symbols_logged::get_method_pointer("UnityEngine.CoreModule.dll", "UnityEngine", "Camera", "set_nearClipPlane", 1);
		auto Unity_get_nearClipPlane_addr = il2cpp_symbols_logged::get_method_pointer("UnityEngine.CoreModule.dll", "UnityEngine", "Camera", "get_nearClipPlane", 0);
		auto Unity_get_farClipPlane_addr = il2cpp_symbols_logged::get_method_pointer("UnityEngine.CoreModule.dll", "UnityEngine", "Camera", "get_farClipPlane", 0);
		auto Unity_set_farClipPlane_addr = il2cpp_symbols_logged::get_method_pointer("UnityEngine.CoreModule.dll", "UnityEngine", "Camera", "set_farClipPlane", 1);
		auto Unity_get_rotation_addr = il2cpp_symbols_logged::get_method_pointer("UnityEngine.CoreModule.dll", "UnityEngine", "Transform", "get_rotation", 0);
		auto Unity_set_rotation_addr = il2cpp_symbols_logged::get_method_pointer("UnityEngine.CoreModule.dll", "UnityEngine", "Transform", "set_rotation", 1);

		auto InvokeMoveNext_addr = il2cpp_symbols::get_method_pointer(
			"UnityEngine.CoreModule.dll", "UnityEngine",
			"SetupCoroutine", "InvokeMoveNext", 2
		);
		/*
		auto Live_SetEnableDepthOfField_addr = il2cpp_symbols::get_method_pointer(
			"PRISM.Legacy.dll", "PRISM",
			"LiveScene", "SetEnableDepthOfField", 1
		);*/

		auto DepthOfFieldClip_CreatePlayable_addr = il2cpp_symbols::get_method_pointer(
			"PRISM.Legacy.dll", "PRISM",
			"DepthOfFieldClip", "CreatePlayable", 2
		);

		/*auto PostProcess_DepthOfFieldClip_CreatePlayable_addr = il2cpp_symbols::get_method_pointer(
			"PRISM.Legacy.dll", "UnityEngine.Rendering.Universal.PostProcess",
			"DepthOfFieldClip", "CreatePlayable", 2
		);*/

		auto Live_Update_addr = il2cpp_symbols::get_method_pointer(
			"PRISM.Legacy.dll", "PRISM",
			"LiveScene", "Update", 0
		);
		/*auto LiveCostumeChangeView_setTryOnMode_addr = il2cpp_symbols::get_method_pointer(
			"PRISM.Interactions.Live.dll", "PRISM.Interactions",
			"LiveCostumeChangeView", "_setTryOnMode", 2
		);
		auto LiveCostumeChangeView_setIdolCostume_addr = il2cpp_symbols::get_method_pointer(
			"PRISM.Interactions.Live.dll", "PRISM.Interactions",
			"LiveCostumeChangeView", "_setIdolCostume", 3
		);

		LiveCostumeChangeModel_get_Idol = reinterpret_cast<decltype(LiveCostumeChangeModel_get_Idol)>(il2cpp_symbols::get_method_pointer(
			"PRISM.Adapters.dll", "PRISM.Adapters",
			"LiveCostumeChangeModel", "get_Idol", 0
		));

		auto LiveCostumeChangeModel_GetDress_addr = il2cpp_symbols::get_method_pointer(
			"PRISM.Adapters.dll", "PRISM.Adapters",
			"LiveCostumeChangeModel", "GetDress", 1
		);
		auto LiveCostumeChangeModel_GetHairstyle_addr = il2cpp_symbols::get_method_pointer(
			"PRISM.Adapters.dll", "PRISM.Adapters",
			"LiveCostumeChangeModel", "GetHairstyle", 1
		);
		auto LiveCostumeChangeModel_GetAccessory_addr = il2cpp_symbols::get_method_pointer(
			"PRISM.Adapters.dll", "PRISM.Adapters",
			"LiveCostumeChangeModel", "GetAccessory", 1
		);
		auto LiveCostumeChangeModel_ctor_addr = il2cpp_symbols::get_method_pointer(
			"PRISM.Adapters.dll", "PRISM.Adapters",
			"LiveCostumeChangeModel", ".ctor", 4
		);*/

		auto AssembleCharacter_AssembleFileCache_method = il2cpp_symbols::find_method(
			"PRISM.Legacy.dll", "PRISM", "AssembleCharacter",
			[](const MethodInfo* method) {
				if (method == nullptr || method->name == nullptr || method->parameters_count != 2)
					return false;
				const auto parameterType = il2cpp_symbols::il2cpp_method_get_param_type_name(method, 1);
				return 0 == strcmp(method->name, "AssembleFileCache") &&
					parameterType != nullptr && 0 == strcmp(parameterType, "UnitIdol");
			});
		auto AssembleCharacter_AssembleFileCache_addr = AssembleCharacter_AssembleFileCache_method != nullptr
			? AssembleCharacter_AssembleFileCache_method->methodPointer : 0;

		auto SpawnCharacter_CreateCharacter_addr = il2cpp_symbols_logged::get_method_pointer(
			"PRISM.Interactions.dll", "PRISM.Interactions",
			"SpawnCharacter", "CreateCharacter", 1
		);

		auto AssembleCharacter_ApplyParam_addr = il2cpp_symbols::get_method_pointer(
			"PRISM.Legacy.dll", "PRISM",
			"AssembleCharacter", "ApplyParam", 6
		);
		/*
		auto AssembleCharacter_ApplyParam_addr = il2cpp_symbols::find_method("PRISM.Legacy.dll", "PRISM", "AssembleCharacter", [=](const MethodInfo* mtd) {
			const std::string mtdName = mtd->name;

			if ((mtdName == "ApplyParam") && (mtd->parameters_count == 6)) {
				if (mtd->methodPointer != AssembleCharacter_ApplyParam_mdl_addr) {
					return true;
				}
			}

			return false;
			});*/

		auto MainThreadDispatcher_LateUpdate_addr = il2cpp_symbols::get_method_pointer(
			"UniRx.dll", "UniRx",
			"MainThreadDispatcher", "LateUpdate", 0
		);

		auto LiveMVUnit_GetMemberChangeRequestData_addr = il2cpp_symbols::get_method_pointer(
			"PRISM.Legacy.dll", "PRISM.Live",
			"LiveMVUnit", "GetMemberChangeRequestData", 3
		);

		auto LiveMVUnitMemberChangePresenter_klass = il2cpp_symbols::get_class("PRISM.Adapters.dll", "PRISM.Adapters", "LiveMvUnitMemberChangePresenter");
		auto LiveMVUnitMemberChangePresenter_c__DisplayClass7_0_klass
			= il2cpp_symbols::find_nested_class_from_name(LiveMVUnitMemberChangePresenter_klass, "<>c__DisplayClass5_0");
		auto LiveMVUnitMemberChangePresenter_c__DisplayClass7_0_klass_initializeAsync_b__4_d_klass
			= il2cpp_symbols::find_nested_class_from_name(LiveMVUnitMemberChangePresenter_c__DisplayClass7_0_klass, "<<InitializeAsync>b__0>d");
		auto LiveMVUnitMemberChangePresenter_initializeAsync_b_4_MoveNext_method =
			il2cpp_class_get_method_from_name(LiveMVUnitMemberChangePresenter_c__DisplayClass7_0_klass_initializeAsync_b__4_d_klass, "MoveNext", 0);
		uintptr_t LiveMVUnitMemberChangePresenter_initializeAsync_b_4_MoveNext_addr = NULL;

		if (LiveMVUnitMemberChangePresenter_initializeAsync_b_4_MoveNext_method) {
			LiveMVUnitMemberChangePresenter_initializeAsync_b_4_MoveNext_addr = LiveMVUnitMemberChangePresenter_initializeAsync_b_4_MoveNext_method->methodPointer;
		}
		else {
			printf("LiveMVUnitMemberChangePresenter_initializeAsync_b_4_MoveNext_method not found.\n");
		}

		auto MvUnitSlotGenerator_NewMvUnitSlot_addr = il2cpp_symbols::get_method_pointer(
			"PRISM.Legacy.dll", "PRISM",
			"MvUnitSlotGenerator", "NewMvUnitSlot", 2
		);

		//auto CheckVocalSeparatedSatisfy_addr = il2cpp_symbols::get_method_pointer(
		//	"PRISM.Legacy.dll", "PRISM.Live",
		//	"MusicData", "CheckVocalSeparatedSatisfy", 1
		//);
		//auto CheckLimitedVocalSeparatedSatisfy_2_addr = il2cpp_symbols::get_method_pointer(
		//	"PRISM.Legacy.dll", "PRISM.Live",
		//	"MusicData", "CheckLimitedVocalSeparatedSatisfy", 2
		//);

		auto CriWareErrorHandler_HandleMessage_addr = il2cpp_symbols::get_method_pointer(
			"CriMw.CriWare.Runtime.dll", "CriWare",
			"CriWareErrorHandler", "HandleMessage", 1
		);

		auto GGIregualDetector_ShowPopup_addr = il2cpp_symbols::get_method_pointer(
			"PRISM.Legacy.dll", "PRISM",
			"GGIregualDetector", "ShowPopup", 1
		);
		auto DMMGameGuard_NPGameMonCallback_addr = il2cpp_symbols::get_method_pointer(
			"PRISM.Legacy.dll", "PRISM",
			"DMMGameGuard", "NPGameMonCallback", 2
		);
		auto DMMGameGuard_Setup_addr = il2cpp_symbols::get_method_pointer(
			"PRISM.Legacy.dll", "PRISM",
			"DMMGameGuard", "Setup", 0
		);
		auto DMMGameGuard_SetCheckMode_addr = il2cpp_symbols::get_method_pointer(
			"PRISM.Legacy.dll", "PRISM",
			"DMMGameGuard", "SetCheckMode", 1
		);
		/*
		CloseNPGameMon = reinterpret_cast<decltype(CloseNPGameMon)>(il2cpp_symbols::get_method_pointer(
			"PRISM.Legacy.dll", "PRISM",
			"DMMGameGuard", "CloseNPGameMon", 0
		));*/

		auto set_fps_addr = il2cpp_symbols_logged::il2cpp_resolve_icall("UnityEngine.Application::set_targetFrameRate(System.Int32)");
		auto set_vsync_count_addr = il2cpp_symbols_logged::il2cpp_resolve_icall("UnityEngine.QualitySettings::set_vSyncCount(System.Int32)");
		auto Unity_Quit_addr = il2cpp_symbols_logged::il2cpp_resolve_icall("UnityEngine.Application::Quit(System.Int32)");


		auto assemblyLoad = reinterpret_cast<void* (*)(Il2CppString*)>(
			il2cpp_symbols::get_method_pointer("mscorlib.dll", "System.Reflection",
				"Assembly", "Load", 1)
			);
		auto assemblyGetType = reinterpret_cast<Il2CppReflectionType * (*)(void*, Il2CppString*)>(
			il2cpp_symbols::get_method_pointer("mscorlib.dll", "System.Reflection",
				"Assembly", "GetType", 1)
			);
		auto reflectionAssembly = assemblyLoad(il2cpp_string_new("mscorlib"));
		auto reflectionType = assemblyGetType(
			reflectionAssembly, il2cpp_string_new("System.Collections.Generic.Dictionary`2[System.Int32, PRISM.Module.Networking.ICostumeStatus]"));
		auto dic_klass = il2cpp_class_from_system_type(reflectionType);
		auto dic_int_ICostumeStatus_add_addr = il2cpp_class_get_method_from_name(dic_klass, "Add", 2)->methodPointer;

		auto GetCostumeListReply_get_CostumeList_addr = il2cpp_symbols::get_method_pointer("PRISM.Module.Networking.dll",
			"PRISM.Module.Networking.Stub.Api", "GetCostumeListReply", "get_CostumeList", 0);
		auto GetCostumeListReply_get_HairstyleList_addr = il2cpp_symbols::get_method_pointer("PRISM.Module.Networking.dll",
			"PRISM.Module.Networking.Stub.Api", "GetCostumeListReply", "get_HairstyleList", 0);
		auto GetCostumeListReply_get_AccessoryList_addr = il2cpp_symbols::get_method_pointer("PRISM.Module.Networking.dll",
			"PRISM.Module.Networking.Stub.Api", "GetCostumeListReply", "get_AccessoryList", 0);

		// [DIAG] v17
		auto CostumePartViewModel_getStatus_addr = il2cpp_symbols::get_method_pointer("PRISM.Adapters.dll",
			"PRISM.Adapters.CostumeChange", "CostumePartViewModel", "_getStatus", 2);
		auto CostumePartViewModel_ctor5_addr = il2cpp_symbols::get_method_pointer("PRISM.Adapters.dll",
			"PRISM.Adapters.CostumeChange", "CostumePartViewModel", ".ctor", 5);

		// [PATCH] v20 - find the accessory ctor (IAccessoryStatus, ISet<int>, ISet<int>).
		// The hairstyle ctor has the same name/arity, so disambiguate by the first
		// parameter's type name.
		uintptr_t CostumePartViewModel_ctorAcc_addr = 0;
		try {
			auto cpvm_klass = ViewDiag::find_class_anywhere("PRISM.Adapters.CostumeChange", "CostumePartViewModel");
			if (cpvm_klass) {
				void* iter = nullptr;
				while (auto mtd = il2cpp_class_get_methods(cpvm_klass, &iter)) {
					if (!mtd->methodPointer) continue;
					const char* nm = il2cpp_method_get_name(mtd);
					if (!nm || strcmp(nm, ".ctor") != 0) continue;
					if (il2cpp_method_get_param_count(mtd) != 3) continue;
					const char* p0n = il2cpp_symbols::il2cpp_method_get_param_type_name(mtd, 0);
					if (p0n && strstr(p0n, "IAccessoryStatus")) {
						CostumePartViewModel_ctorAcc_addr = (uintptr_t)mtd->methodPointer;
						break;
					}
				}
			}
		}
		catch (...) {}
		printf("CostumePartViewModel_ctorAcc found at 0x%p\n", (void*)CostumePartViewModel_ctorAcc_addr);

		// [DIAG] v20 - which flag the footer flips when an accessory is selected.
		auto CostumeChangeFooterView_SetShowDecide_addr = (uintptr_t)ViewDiag::find_method_anywhere(
			"PRISM.Interactions.CostumeChange", "CostumeChangeFooterView", "SetShowDecide", 1
		);

		auto LiveMVUnitConfirmationModel_ctor_addr = il2cpp_symbols::get_method_pointer(
			"PRISM.Service.dll", "PRISM.Service.Live",
			"LiveMvUnitConfirmationModel", ".ctor", 5
		);

		auto RunwayUnitConfirmationModel_ctor_addr = il2cpp_symbols::get_method_pointer(
			"PRISM.Adapters.dll", "PRISM.Adapters",
			"RunwayUnitConfirmationModel", ".ctor", 6
		);

		auto SwayString_SetupPoint_addr = il2cpp_symbols::get_method_pointer(
			"PRISM.Legacy.dll", "PRISM",
			"SwayString", "SetupPoint", 0
		);

		auto CostumeChangeViewModel_ctor_addr = il2cpp_symbols::get_method_pointer(
			"PRISM.Adapters.dll", "PRISM.Adapters.CostumeChange",
			"CostumeChangeViewModel", ".ctor", 6
		);

		auto LiveMVStartData_ctor_addr = il2cpp_symbols::get_method_pointer(
			"PRISM.Legacy", "PRISM.Live",
			"LiveMVStartData", ".ctor", 9
		);


		auto LiveStartDataExtensions_PreLoadAsync_addr = il2cpp_symbols_logged::get_method_pointer(
			"PRISM.Legacy.dll", "PRISM.Live",
			"LiveStartDataExtensions", "PreLoadAsync", 1
		);

		auto Subject_OnNext_addr = GetSubject_OnNext_addr();

		auto MagicaCloth_BuildAndRun_addr = il2cpp_symbols_logged::get_method_pointer(
			"MagicaClothV2.dll", "MagicaCloth2",
			"MagicaCloth", "BuildAndRun", 0
		);

		auto MagicaCloth_SetParameterChange_addr = il2cpp_symbols_logged::get_method_pointer(
			"MagicaClothV2.dll", "MagicaCloth2",
			"MagicaCloth", "SetParameterChange", 0
		);

		auto MagicaClothController_get_Inertia_addr = il2cpp_symbols_logged::get_method_pointer(
			"PRISM.Module.CustomMagicaCloth.dll", "PRISM.Module.CustomMagicaCloth",
			"MagicaClothController", "get_Inertia", 0
		);

		auto MagicaClothController_get_Radius_addr = il2cpp_symbols_logged::get_method_pointer(
			"PRISM.Module.CustomMagicaCloth.dll", "PRISM.Module.CustomMagicaCloth",
			"MagicaClothController", "get_Radius", 0
		);

		auto BodyParamFloatProperty__ctor_addr = il2cpp_symbols_logged::get_method_pointer(
			"PRISM.Module.CustomMagicaCloth.dll", "PRISM.Module.CustomMagicaCloth",
			"BodyParamFloatProperty", ".ctor", 0
		);

		auto BodyParamFloatProperty__getValue_addr = il2cpp_symbols_logged::get_method_pointer(
			"PRISM.Module.CustomMagicaCloth.dll", "PRISM.Module.CustomMagicaCloth",
			"BodyParamFloatProperty", "_getValue", 1
		);

		auto MagicaClothController_Awake_addr = il2cpp_symbols_logged::get_method_pointer(
			"PRISM.Module.CustomMagicaCloth.dll", "PRISM.Module.CustomMagicaCloth",
			"MagicaClothController", "Awake", 0
		);

		auto CostumeChangeViewModel_Apply_addr = il2cpp_symbols_logged::get_method_pointer(
			"PRISM.Adapters.dll", "PRISM.Adapters.CostumeChange", "CostumeChangeViewModel", "Apply", 0
		);
		fp_CostumeChangeViewModel_Apply = reinterpret_cast<void(*)(void*)>(CostumeChangeViewModel_Apply_addr);

		auto CostumeChangeViewModel__ctor_addr = il2cpp_symbols_logged::get_method_pointer(
			"PRISM.Adapters.dll", "PRISM.Adapters.CostumeChange",
			"CostumeChangeViewModel", ".ctor", 6
		);

		auto CostumeChangeViewModel_CanDecide0_addr = il2cpp_symbols_logged::get_method_pointer(
			"PRISM.Adapters.dll", "PRISM.Adapters.CostumeChange",
			"CostumeChangeViewModel", "CanDecide", 0
		);

		auto CostumeChangeViewModel_CanDecide2_addr = il2cpp_symbols_logged::get_method_pointer(
			"PRISM.Adapters.dll", "PRISM.Adapters.CostumeChange",
			"CostumeChangeViewModel", "CanDecide", 2
		);

#pragma endregion
		ADD_HOOK(SetResolution, "SetResolution at %p");
		ADD_HOOK_1(StoryExtensions_IsLocked);
		ADD_HOOK_1(DioramaProductViewModel_IsLocked);
		ADD_HOOK_1(RunwayPoseSettingContentViewModel_ctor);
		ADD_HOOK(LocalizationManager_GetTextOrNull, "LocalizationManager_GetTextOrNull at %p");
		ADD_HOOK(GetResolutionSize, "GetResolutionSize at %p");
		ADD_HOOK(AssetBundle_LoadAsset, "AssetBundle_LoadAsset at %p");
		ADD_HOOK(LiveMVOverlayView_UpdateLyrics, "LiveMVOverlayView_UpdateLyrics at %p");
		ADD_HOOK(TimelineController_SetLyric, "TimelineController_SetLyric at %p");
		ADD_HOOK(get_baseCamera, "get_baseCamera at %p");
		ADD_HOOK(Unity_get_position, "Unity_get_position at %p");
		ADD_HOOK(Unity_set_position, "Unity_set_position at %p");
		ADD_HOOK(Unity_get_fieldOfView, "Unity_get_fieldOfView at %p");
		ADD_HOOK(Unity_set_fieldOfView, "Unity_set_fieldOfView at %p");
		ADD_HOOK(Unity_InternalLookAt, "Unity_InternalLookAt at %p");
		ADD_HOOK(Unity_set_nearClipPlane, "Unity_set_nearClipPlane at %p");
		ADD_HOOK(Unity_get_nearClipPlane, "Unity_get_nearClipPlane at %p");
		ADD_HOOK(Unity_get_farClipPlane, "Unity_get_farClipPlane at %p");
		ADD_HOOK(Unity_set_farClipPlane, "Unity_set_farClipPlane at %p");
		ADD_HOOK(Unity_get_rotation, "Unity_get_rotation at %p");
		ADD_HOOK(Unity_set_rotation, "Unity_set_rotation at %p");

		ADD_HOOK(TMP_Text_set_text, "TMP_Text_set_text at %p");
		ADD_HOOK(UITextMeshProUGUI_Awake, "UITextMeshProUGUI_Awake at %p");
		ADD_HOOK(ScenarioManager_Init, "ScenarioManager_Init at %p");
		ADD_HOOK(DataFile_GetBytes, "DataFile_GetBytes at %p");
		//ADD_HOOK(UnsafeLoadBytesFromKey, "UnsafeLoadBytesFromKey at %p");
		ADD_HOOK(TextLog_AddLog, "TextLog_AddLog at %p");
		ADD_HOOK(InvokeMoveNext, "InvokeMoveNext at %p");
		// ADD_HOOK(Live_SetEnableDepthOfField, "Live_SetEnableDepthOfField at %p");
		ADD_HOOK(DepthOfFieldClip_CreatePlayable, "DepthOfFieldClip_CreatePlayable at %p");
		//ADD_HOOK(PostProcess_DepthOfFieldClip_CreatePlayable, "PostProcess_DepthOfFieldClip_CreatePlayable at %p");
		// ADD_HOOK(Live_Update, "Live_Update at %p");
		//ADD_HOOK(LiveCostumeChangeView_setTryOnMode, "LiveCostumeChangeView_setTryOnMode at %p");
		//ADD_HOOK(LiveCostumeChangeView_setIdolCostume, "LiveCostumeChangeView_setIdolCostume at %p");
		//ADD_HOOK(LiveCostumeChangeModel_GetDress, "LiveCostumeChangeModel_GetDress at %p");
		//ADD_HOOK(LiveCostumeChangeModel_GetHairstyle, "LiveCostumeChangeModel_GetHairstyle at %p");
		//ADD_HOOK(LiveCostumeChangeModel_GetAccessory, "LiveCostumeChangeModel_GetAccessory at %p");
		//ADD_HOOK(LiveCostumeChangeModel_ctor, "LiveCostumeChangeModel_ctor at %p");
		ADD_HOOK_1(AssembleCharacter_AssembleFileCache);
		ADD_HOOK_1(SpawnCharacter_CreateCharacter);
		ADD_HOOK(AssembleCharacter_ApplyParam, "AssembleCharacter_ApplyParam at %p");
		ADD_HOOK(MainThreadDispatcher_LateUpdate, "MainThreadDispatcher_LateUpdate at %p");
		// [PATCH] enabled - these feed checkAndAddCostume() so every costume the server
		// sends gets isUnlocked_ = true (the "real" unlock the preview screen shows).
		ADD_HOOK(dic_int_ICostumeStatus_add, "dic_int_ICostumeStatus_add at %p");
		ADD_HOOK(GetCostumeListReply_get_CostumeList, "GetCostumeListReply_get_CostumeList at %p");
		ADD_HOOK(GetCostumeListReply_get_HairstyleList, "GetCostumeListReply_get_HairstyleList at %p");
		ADD_HOOK(GetCostumeListReply_get_AccessoryList, "GetCostumeListReply_get_AccessoryList at %p");
		// [DIAG] v17 - trace the part-level unlock decision in the costume-change UI
		ADD_HOOK(CostumePartViewModel_getStatus, "CostumePartViewModel_getStatus at %p");
		ADD_HOOK(CostumePartViewModel_ctor5, "CostumePartViewModel_ctor5 at %p");
		// [PATCH] v20 - accessory parts constructed as Unlocked + footer decide fix.
		if (CostumePartViewModel_ctorAcc_addr) {
			ADD_HOOK(CostumePartViewModel_ctorAcc, "CostumePartViewModel_ctorAcc at %p");
		}
		if (CostumeChangeFooterView_SetShowDecide_addr) {
			ADD_HOOK_1(CostumeChangeFooterView_SetShowDecide);
		}
		ADD_HOOK(LiveMVUnitConfirmationModel_ctor, "LiveMVUnitConfirmationModel_ctor at %p");
		ADD_HOOK(RunwayUnitConfirmationModel_ctor, "RunwayUnitConfirmationModel_ctor at %p");
		ViewDiag::InstallHooks(); // [DIAG] v15 - view connector / viewMapping diagnostics
		ADD_HOOK(SwayString_SetupPoint, "SwayString_SetupPoint at %p");
		ADD_HOOK(LiveMVUnit_GetMemberChangeRequestData, "LiveMVUnit_GetMemberChangeRequestData at %p");
		ADD_HOOK(LiveMVUnitMemberChangePresenter_initializeAsync_b_4_MoveNext, "LiveMVUnitMemberChangePresenter_initializeAsync_b_4_MoveNext at %p");
		ADD_HOOK(MvUnitSlotGenerator_NewMvUnitSlot, "MvUnitSlotGenerator_NewMvUnitSlot at %p");
		//ADD_HOOK(CheckVocalSeparatedSatisfy, "CheckVocalSeparatedSatisfy at %p");
		//ADD_HOOK(CheckLimitedVocalSeparatedSatisfy_2, "CheckLimitedVocalSeparatedSatisfy_2 at %p");
		ADD_HOOK(CriWareErrorHandler_HandleMessage, "CriWareErrorHandler_HandleMessage at %p");
		ADD_HOOK(GGIregualDetector_ShowPopup, "GGIregualDetector_ShowPopup at %p");
		ADD_HOOK(DMMGameGuard_NPGameMonCallback, "DMMGameGuard_NPGameMonCallback at %p");
		// ADD_HOOK(DMMGameGuard_Setup, "DMMGameGuard_Setup at %p");
		ADD_HOOK(DMMGameGuard_SetCheckMode, "DMMGameGuard_SetCheckMode at %p");
		ADD_HOOK(set_fps, "set_fps at %p");
		ADD_HOOK(set_vsync_count, "set_vsync_count at %p");
		ADD_HOOK(Unity_Quit, "Unity_Quit at %p");

		ADD_HOOK(LiveMVStartData_ctor, "LiveMVStartData_ctor at %p");
		ADD_HOOK_1(LiveStartDataExtensions_PreLoadAsync);

		ADD_HOOK_1(Subject_OnNext);
		ADD_HOOK_1(MagicaCloth_BuildAndRun);
		ADD_HOOK_1(MagicaCloth_SetParameterChange);
		ADD_HOOK_1(MagicaClothController_get_Inertia);
		ADD_HOOK_1(MagicaClothController_get_Radius);
		//ADD_HOOK_1(BodyParamFloatProperty__ctor);
		//ADD_HOOK_1(BodyParamFloatProperty__getValue);
		ADD_HOOK_1(MagicaClothController_Awake);

		ADD_HOOK_1(CostumeChangeViewModel__ctor);
		ADD_HOOK_1(CostumeChangeViewModel_CanDecide0);
		ADD_HOOK_1(CostumeChangeViewModel_CanDecide2);
		ADD_HOOK_ADDR(PRISM.Adapters.dll, PRISM.Adapters.CostumeChange, CostumeChangeViewModel, RefreshViewModels, 0);
		// [PATCH] v7 - limited-time gate bypass (see the hooks above).
		// Resolve at runtime so a class that has no such getter is harmless.
#define INSTALL_LIMITED_TIME_GATE(klass) \
		{ \
			auto k = il2cpp_symbols_logged::get_class("PRISM.Definitions.dll", "PRISM.Definitions", #klass); \
			auto m = k ? il2cpp_class_get_method_from_name(k, "get_IsLimitedTimeUnlock", 0) : nullptr; \
			if (m && m->methodPointer) { \
				MH_CreateHook((void*)m->methodPointer, (void*)klass##_get_IsLimitedTimeUnlock_hook, &klass##_get_IsLimitedTimeUnlock_orig); \
				MH_EnableHook((void*)m->methodPointer); \
				printf("[v7] limited-time gate OFF on %s\n", #klass); \
			} \
			else { \
				printf("[v7] %s has no get_IsLimitedTimeUnlock\n", #klass); \
			} \
		}
		INSTALL_LIMITED_TIME_GATE(MstCostume)
		INSTALL_LIMITED_TIME_GATE(MstHairstyle)
		INSTALL_LIMITED_TIME_GATE(MstAccessory)
		INSTALL_LIMITED_TIME_GATE(MstProduceIdol)
		INSTALL_LIMITED_TIME_GATE(MstIdol)
		INSTALL_LIMITED_TIME_GATE(MstCharacterInfo)
		DumpMasterFields();
		// [PATCH] v9 - capture the client's error report (see the hook above)
		ADD_HOOK_ADDR(PRISM.Module.Networking.dll, PRISM.Module.Networking, ErrorHandler, Handle, 1);

		// [PATCH] master row dump for MstProduceIdol (see the hooks above)
		{
			std::ofstream f("master-dump.txt", std::ios::app);
			if (f.is_open()) f << "=== dump patch v7 active ===\n";
		}
		ADD_HOOK_ADDR(PRISM.Definitions.dll, PRISM.Definitions, MstProduceIdol, get_Id, 0);
		ADD_HOOK_ADDR(PRISM.Definitions.dll, PRISM.Definitions, MstProduceIdol, get_MstIdolId, 0);
		ADD_HOOK_ADDR(PRISM.Definitions.dll, PRISM.Definitions, MstProduceIdol, set_Id, 1);
		ADD_HOOK_ADDR(PRISM.Definitions.dll, PRISM.Definitions, MstProduceIdol, set_MstIdolId, 1);

		// permanent limited-time events: v14 - one-shot field WRITE on the
		// newest Shiny Runway row (see EventDates::ApplyPatch).  No hooks:
		// the four getter hooks were removed in v13 (their addresses are
		// ICF-shared with unrelated methods - hooking them crashed the game).
		// This block still installs nothing; it only reports the method
		// addresses and backing field offsets to runway-diag.txt.  The
		// applied dates are logged to mst-events-patch.txt.
		{
			auto k = il2cpp_symbols_logged::get_class("PRISM.Definitions.dll", "PRISM.Definitions", "MstEvent");
			std::ofstream d("runway-diag.txt", std::ios::app);
			if (d.is_open()) {
				d << "=== v14 EventDates diag (pid " << GetCurrentProcessId() << ") - no hooks, write patch in EventDates ===\n";
				d << "MstEvent klass=0x" << std::hex << (uintptr_t)k << std::dec;
				if (k) {
					const char* kn = il2cpp_class_get_name(k);
					const char* kns = il2cpp_class_get_namespace(k);
					d << " name=" << (kn ? kn : "?") << " ns=" << (kns ? kns : "?");
				}
				d << "\n";
			}
			auto reportOne = [&](const char* name, int argc) {
				auto m = k ? il2cpp_class_get_method_from_name(k, name, argc) : nullptr;
				if (!d.is_open()) return;
				d << name << "(" << argc << ")";
				if (m) {
					d << " mi=0x" << std::hex << (uintptr_t)m
						<< " ptr=0x" << (uintptr_t)m->methodPointer << std::dec;
					auto rt = il2cpp_method_get_return_type(m);
					if (rt) {
						const char* tn = il2cpp_type_get_name(rt);
						if (tn) d << " ret=" << tn;
					}
				}
				else d << " (not found)";
				d << "\n";
			};
			reportOne("get_Id", 0);
			reportOne("get_EventType", 0);
			reportOne("get_BeginDate", 0);
			reportOne("get_EndDate", 0);
			reportOne("get_DisplayEndDate", 0);
			reportOne("set_DisplayEndDate", 1);
			reportOne("DisplayEndDate", 1);
			static const char* kBacking[] = {
				"<Id>k__BackingField", "<EventType>k__BackingField",
				"<BeginDate>k__BackingField", "<EndDate>k__BackingField",
				"<DisplayEndDate>k__BackingField" };
			if (d.is_open() && k) {
				for (const char* bn : kBacking) {
					auto fi = il2cpp_symbols_logged::il2cpp_class_get_field_from_name(k, bn);
					d << "field " << bn << ": ";
					if (fi) d << "offset=" << fi->offset;
					else d << "(not found)";
					d << "\n";
				}
				d.flush();
			}
		}

		tools::AddNetworkingHooks();

		LoadReplacementTextures();
		printf("%zu textures are loaded to replace.\n", replacementTexureNames.size());

		on_hotKey_0 = []() {
			startSCGUI();
			// needPrintStack = !needPrintStack;
			};
		SCCamera::initCameraSettings();
		g_on_hook_ready();

		const auto gameVersionInfo = getGameVersions();
		wprintf(L"Plugin Loaded - Game Version: %ls, Resource Version: %ls\n", gameVersionInfo.gameVersion.c_str(), gameVersionInfo.resourceVersion.c_str());
	}
}

void uninit_hook()
{
	if (!mh_inited)
		return;

	MH_DisableHook(MH_ALL_HOOKS);
	MH_Uninitialize();
}

bool init_hook()
{
	if (mh_inited)
		return false;

	if (MH_Initialize() != MH_OK)
		return false;

	g_on_close = []() {
		uninit_hook();
		TerminateProcess(GetCurrentProcess(), 0);
		};

	mh_inited = true;

	MH_CreateHook(LoadLibraryW, load_library_w_hook, &load_library_w_orig);
	MH_EnableHook(LoadLibraryW);
	return true;
}
