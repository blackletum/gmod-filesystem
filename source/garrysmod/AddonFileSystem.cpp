#include "garrysmod/AddonFileSystem.h"
#include "garrysmod/DedicatedServerAddons.h"
#include "garrysmod/public/IAddonDownloadNotify.h"
#include "garrysmod/public/IMenuSystem.h"
#include "garrysmod/public/IGet.h"
#include "garrysmod/tasks/Tasks.h"
#include "garrysmod/AddonReader.h"
#include "tier1/keyvalues.h"
#include "sdk_backports.h"
#include "basefilesystem.h"
#include <charconv>

void Addon::FileSystem::Clear()
{
	Msg( "Addon::FileSystem::Clear\n" );
	for ( auto &pAddon : m_MountedAddons )
	{
		if ( pAddon.m_hFileHandle != FILESYSTEM_INVALID_HANDLE )
			g_pFullFileSystem->Close( pAddon.m_hFileHandle );
	}

	m_MountedAddons.clear();
	m_Folders.clear();
}

void Addon::FileSystem::Refresh()
{
	Msg( "Addon::FileSystem::Refresh\n" );

	UpdateModPath();
	for ( IAddonSystem::Information &info : m_Addons )
	{
		if ( MountAddon( info ) )
			continue;

		if ( !info.downloaded || info.file.find( "cache/" ) == std::string::npos )
			continue;

		Warning( "Removing bad addon %s\n\n", info.file.c_str() );
		g_pFullFileSystem->RemoveFile( info.file.c_str(), "MOD" );
	}

	Load();
}

Addon::Folder* Addon::FileSystem::GetFolder( const std::string &strPath, bool bCreate )
{
	std::string strCleanPath = strPath;
	Bootil::String::File::CleanPath( strCleanPath );

	auto it = m_Folders.find( strCleanPath );
	if ( it != m_Folders.end() )
		return &it->second;

	if ( !bCreate )
		return nullptr;

	it = m_Folders.emplace( strCleanPath, std::map<std::string, FileInfo>{} ).first;
	return &it->second;
}

// RaphaelIT7 (ToDo):
// It currently crashes after the last call made here, idk why.
bool Addon::FileSystem::MountFile( const std::string& gmaPath, std::vector<std::string>* files, uint64_t wsid, uint64_t wsid2, IAddonSystem::AddonSource source )
{
	Msg( "Addon::FileSystem::MountFile (%s - %llu - %llu - %i)\n", gmaPath.c_str(), wsid, wsid2, source.m_UnknownValue );

	std::string strAddonPath = gmaPath;
	Bootil::String::Lower( strAddonPath );

	static const std::string strContentPrefix = "content/4000/";
	if ( Bootil::String::Test::StartsWith( strAddonPath, strContentPrefix ) )
	{
		const size_t slash = strAddonPath.find( '/', strContentPrefix.length() );
		if ( slash != std::string::npos )
		{
			const std::string strWSID = strAddonPath.substr( strContentPrefix.length(), slash - strContentPrefix.length() );
			const uint64_t nContentWSID = Bootil::String::To::UInt64( strWSID );
			if ( nContentWSID != 0 )
			{
				strAddonPath = GetAddonFilepath( nContentWSID, true );
				wsid = nContentWSID;
			}
		}
	}

	std::string lowerAddonPath = strAddonPath;
	Bootil::String::Lower( lowerAddonPath );

	bool alreadyMounted = false;
	for ( auto it = m_MountedAddons.begin(); it != m_MountedAddons.end(); ++it )
	{
		const std::string& mountedPath = it->m_strPath;
		if ( Bootil::String::Test::EndsWith( lowerAddonPath, mountedPath ) )
		{
			alreadyMounted = true;
			break;
		}
	}

	if ( alreadyMounted )
	{
		if ( source == IAddonSystem::AddonSource( 1 ) )
		{
			Msg( "Addon '%s' is already mounted, ignoring...\n", strAddonPath.c_str() );
			return true;
		}
	}

	Addon::Reader reader( wsid2 );
	if ( !reader.OpenFile( strAddonPath ) )
	{
		Warning( "Couldn't mount file [%s]\n", strAddonPath.c_str() );
		return false;
	}

	FileHandle_t hFileHandle = reader.GetPackFile();
	if ( !hFileHandle )
	{
		Warning( "Couldn't mount file [%s] (invalid pack file)\n", strAddonPath.c_str() );
		return false;
	}

	const bool addonFlag = !alreadyMounted &&
		( source == IAddonSystem::AddonSource( 1 ) || source == IAddonSystem::AddonSource( 2 ) );

	MountedAddon addon;
	addon.m_strPath = strAddonPath;
	addon.m_strTitle = "";
	addon.m_hFileHandle = hFileHandle;
	addon.m_nWsid = wsid;
	addon.m_nWsid2 = wsid2;
	addon.m_bDeleteOnUnmount = addonFlag;

	m_MountedAddons.push_back( std::move( addon ) );

	for ( uint32_t i=0; i<reader.GetNumFiles(); ++i )
	{
		Addon::FileEntry& fileEntry = reader.GetFile( i );

		std::string strFixedFolder = fileEntry.strName;
		Bootil::String::File::FixSlashes( strFixedFolder );
		Bootil::String::File::StripFilename( strFixedFolder );

		Folder* pFolder = GetFolder( strFixedFolder, true );

		std::string strFileName = fileEntry.strName;
		Bootil::String::File::ExtractFilename( strFileName );

		FileInfo info;
		info.m_strFileName = strFileName;
		info.m_strFolderName = strFixedFolder;
		info.m_nSize = fileEntry.iSize;
		info.m_nOffset = fileEntry.iOffset;
		info.m_hFileHandle = hFileHandle;
		info.m_nWsid = wsid;

		if ( files )
			files->push_back( strFileName );

		auto existing = pFolder->find( strFileName );
		if ( existing != pFolder->end() )
		{
			FileInfo& oldInfo = existing->second;
			if ( oldInfo.m_nWsid != wsid && Bootil::String::Test::EndsWith( strFileName, ".lua" ) )
				Warning( "Addon '%s' (%llu) contains file from %llu: '%s'\n", strAddonPath.c_str(), wsid , oldInfo.m_nWsid , oldInfo.m_strFileName.c_str() );

			existing->second = std::move( info );
		}
		else
			pFolder->emplace( strFileName, std::move( info ) );
	}

	reader.ExtractFiles();
	return true;
}

bool Addon::FileSystem::ShouldMount( uint64_t wsid )
{
	Msg( "Addon::FileSystem::ShouldMount (%llu)\n", wsid );
	auto it = m_AddonNoMount.find( wsid );
	if ( it != m_AddonNoMount.end() )
		return false;

	for ( IAddonSystem::Information info : m_Addons )
	{
		if ( info.wsid == wsid )
			return true;
	}

	Msg( "CAddonFileSystem::ShouldMount Nope? (%llu)\n", wsid );

	return false;
}

// RaphaelIT7 (Verify): Does it truly return bShouldMount? Seems like that in IDA for both linux & macos
bool Addon::FileSystem::SetShouldMount( uint64_t wsid, bool bShouldMount )
{
	Msg( "Addon::FileSystem::SetShouldMount (%llu - %s)\n", wsid, bShouldMount ? "true" : "false" );

	auto it = m_AddonNoMount.find( wsid );
	if (it != m_AddonNoMount.end())
	{
		if (bShouldMount)
			m_AddonNoMount.erase(it);
	} else if (!bShouldMount)
		m_AddonNoMount.insert( wsid );

	return bShouldMount;
}

void Addon::FileSystem::Save()
{
	Msg( "CAddonFileSystem::Save\n" );
	KeyValues* kv = new KeyValues( "addonnomount" );
	RunCodeAtScopeExit( kv->deleteThis(); );

	KeyValues* list = kv->CreateNewKey();

	int i = 0;
	char szIDBuffer[21];
	char szWSIDBuffer[21];
	for ( auto wsid : m_AddonNoMount )
	{
		++i;
		auto [wsPtr, _] = std::to_chars( szWSIDBuffer, szWSIDBuffer + sizeof( szWSIDBuffer ) - 1, wsid );
		*wsPtr = '\0';

		auto [idPtr, _2] = std::to_chars( szIDBuffer, szIDBuffer + sizeof( szIDBuffer ) - 1, i );
		*idPtr = '\0';

		list->SetString( szIDBuffer, szWSIDBuffer );
	}

	kv->SaveToFile( g_pFullFileSystem, "cfg/addonnomount.txt", "DEFAULT_WRITE_PATH" );
}

const std::list<IAddonSystem::Information>& Addon::FileSystem::GetList() const
{
	Msg( "CAddonFileSystem::GetList\n" );

	return m_Addons;
}

const std::list<IAddonSystem::UGCInfo>& Addon::FileSystem::GetUGCList() const
{
	Msg( "CAddonFileSystem::GetUGCList\n" );
	return m_UgcAddons;
}

void Addon::FileSystem::ScanForSubscriptions( const char *unknown1, bool unknown2 ) // NOTE: Gmod uses the Steamworks 1.57. The sourcesdk-minimal was outdated.
{
	Msg( "CAddonFileSystem::ScanForSubscriptions (%s - %s)\n", unknown1, unknown2 ? "true" : "false" );
	if (!get)
	{
		Error( "SFS: !get" );
		return;
	}

	if ( get->IsDedicatedServer() )
	{
		Warning( "   ^-- TODO: GarrysMod::DedicatedServer::RunAddonProcess!\n" );
		GarrysMod::DedicatedServer::RunAddonProcess( unknown1 );
		MountFloatingAddons();
		return;
	}
	else
	{
		m_CallbackSubscribed.Register( this, &Addon::FileSystem::OnRemoteStoragePublishedFileSubscribed );
		m_CallbackUnsubscribed.Register( this, &Addon::FileSystem::OnRemoteStoragePublishedFileUnsubscribed );

		AddJob( new Addon::Task::AddFloatingAddons() );
		AddJob( new Addon::Task::GetSubscriptions() );
		AddJob( new Addon::Task::MountAvailable() );

		if ( !IsOfflineMode() )
		{
			AddJob( new Addon::Task::DownloadAddons( true ) );
			// RaphaelIT7: There seems to be another call here though idk what that one is
			SendUGCListUpdate();
		}

		Think();
	}
}

void Addon::FileSystem::OnRemoteStoragePublishedFileSubscribed( RemoteStoragePublishedFileSubscribed_t *info )
{
	if ( IsOfflineMode() ) 
		return;

	if ( info->m_nAppID != SteamUtils()->GetAppID() ) 
		return;

	if ( !IsSubscribed( info->m_nPublishedFileId ) )
		AddJob( new Addon::Task::OnSubscribed( info->m_nPublishedFileId ) ); 
}

void Addon::FileSystem::OnRemoteStoragePublishedFileUnsubscribed( RemoteStoragePublishedFileUnsubscribed_t *info )
{
	if ( IsOfflineMode() )
		return;

	if ( info->m_nAppID != SteamUtils()->GetAppID() )
		return;

	for ( auto it = m_MountedAddons.begin(); it != m_MountedAddons.end(); ++it )
	{
		if ( it->m_nWsid != info->m_nPublishedFileId )
			continue;

		UnmountFile( it->m_strPath, "addon unsubscribed" );
		if ( it->m_bDeleteOnUnmount )
			g_pFullFileSystem->RemoveFile( it->m_strPath.c_str(), "MOD" );

		EnableLoadingUnloadedAddons(); // ???
		// Some other call here?
		AddJob( new Addon::Task::MountAvailable() );
		SendUGCListUpdate();
		return;
	}

	for ( auto it = m_UgcAddons.begin(); it != m_UgcAddons.end(); ++it )
	{
		if ( it->wsid != info->m_nPublishedFileId )
			continue;

		m_UgcAddons.erase( it );
		break;
	}

	SendUGCListUpdate();
}


Addon::FileSystem::FileSystem() :
	m_CallbackSubscribed( this, &Addon::FileSystem::OnRemoteStoragePublishedFileSubscribed ),
	m_CallbackUnsubscribed( this, &Addon::FileSystem::OnRemoteStoragePublishedFileUnsubscribed )
{
}

void Addon::FileSystem::UpdateModPath()
{
	m_strModPath = get->GameDir();
	m_strModPath.append( "\\workshop\\" );

	if ( fs_tellmeyoursecrets.GetBool() )
		Msg( "Addon[UpdateModPath]: ModPath [%s]\n", m_strModPath.c_str() );

	Bootil::String::File::FixSlashes( m_strModPath );
	Bootil::String::Lower( m_strModPath );

	if ( fs_tellmeyoursecrets.GetBool() )
		Msg( "Addon[UpdateModPath]: Cleaned [%s]\n", m_strModPath.c_str() );
}

// RaphaelIT7:
// Oh my god this is horrible
// I get what GMod is doing now.
// So they hook into HandleOpenRegularFile and try to match ANY entry until the workshop/ searchpath is tried...
// and only in that case this will match and can hit a Addon file.
void Addon::FileSystem::NormalizePath( std::string& strFileName )
{
	Bootil::String::File::FixSlashes( strFileName );
	Bootil::String::Lower( strFileName );
	Bootil::String::Util::Trim( strFileName );
	Bootil::String::Util::FindAndReplace( strFileName, "//", "/" );

	if ( strFileName.length() > m_strModPath.length() && Bootil::String::Test::StartsWith( strFileName, m_strModPath ) )
		strFileName.assign( strFileName.c_str() + m_strModPath.length() );
}

void Addon::FileSystem::SendUGCListUpdate()
{
	if ( Notify() )
		Notify()->NotifySubscriptionChanges();
}

bool Addon::FileSystem::UnmountFile( std::string strFileName, const char *pszReason )
{
	for ( auto it = m_MountedAddons.begin(); it != m_MountedAddons.end(); ++it )
	{
		if ( it->m_strPath != strFileName )
			continue;

		Warning( "Unmounting (%s) '%s'\n", pszReason, strFileName.c_str() );
		// UnmountPackFile( it->m_hFileHandle, false );
		return true;
	}

	return false;
}

bool Addon::FileSystem::MountAddon( IAddonSystem::Information &info )
{
	Msg( "Addon::FileSystem::MountAddon (%llu)\n", info.wsid );

	if ( info.downloaded )
	{
		if ( info.canUpdate )
		{
			std::string outdatedPath = info.file + ".outdated";
			if ( g_pFullFileSystem->FileExists( outdatedPath.c_str(), "MOD" ) )
				g_pFullFileSystem->RemoveFile( outdatedPath.c_str(), "MOD" );
		}

		if ( !SetShouldMount( info.wsid, info.downloaded ) )
			return true;

		info.file = GetAddonFilepath( info.wsid, false );
	}

	if ( info.file.empty() )
	{
		info.failure = "Missing file";
		Warning( "Addon '%s' (%llu) doesn't have a file, nothing to mount...\n", info.title.c_str(), info.wsid );
		if ( get->MenuSystem() ) // RaphaelIT7: really? passing info?
			get->MenuSystem()->SendProblemToMenu( "missing_addon_file", 2, (const char*)&info );

		return false;
	}

	Addon::Reader reader( info.hcontent_file );
	if ( !reader.OpenFile( info.file ) )
	{
		info.failure = "Failed to parse addon file";
		Warning( "Couldn't mount addon file '%s' from '%s' (%llu)\n", info.file.c_str(), info.title.c_str(), info.wsid );
		return false;
	}

	FileHandle_t hFileHandle = reader.GetPackFile();
	if ( !hFileHandle )
	{
		info.failure = "Failed to open addon file";
		Warning( "Failed to open addon file '%s', not mounting!\n", info.file.c_str() );
		return false;
	}

	MountedAddon addon;
	addon.m_strPath = info.file;
	addon.m_strTitle = info.title;
	addon.m_hFileHandle = hFileHandle;
	addon.m_nWsid = info.wsid;
	addon.m_nWsid2 = 0;
	addon.m_bDeleteOnUnmount = false;

	m_MountedAddons.push_back( std::move( addon ) );

	for ( uint32_t i=0; i<reader.GetNumFiles(); ++i )
	{
		Addon::FileEntry& fileEntry = reader.GetFile( i );

		std::string strFixedFolder = fileEntry.strName;
		Bootil::String::File::FixSlashes( strFixedFolder );
		Bootil::String::File::StripFilename( strFixedFolder );

		Folder* pFolder = GetFolder( strFixedFolder, true );

		std::string strFileName = fileEntry.strName;
		Bootil::String::File::ExtractFilename( strFileName );

		FileInfo fileInfo;
		fileInfo.m_strFileName = strFileName;
		fileInfo.m_strFolderName = strFixedFolder;
		fileInfo.m_nSize = fileEntry.iSize;
		fileInfo.m_nOffset = fileEntry.iOffset;
		fileInfo.m_hFileHandle = hFileHandle;
		fileInfo.m_nWsid = info.wsid;

		if ( Bootil::String::Test::EndsWith( strFileName, ".mdl" ) )
			++info.model_count;

		auto existing = pFolder->find( strFileName );
		if ( existing != pFolder->end() )
		{
			FileInfo& oldInfo = existing->second;
			if ( oldInfo.m_nWsid != info.wsid && Bootil::String::Test::EndsWith( strFileName, ".lua" ) )
				Warning( "Addon '%s' (%llu) contains file from %llu: '%s'\n", info.title.c_str(), info.wsid, oldInfo.m_nWsid, oldInfo.m_strFileName.c_str() );

			existing->second = std::move( fileInfo );
		}
		else
			pFolder->emplace( strFileName, std::move( fileInfo ) );
	}

	reader.ExtractFiles();
	return true;
}

std::string Addon::FileSystem::ResolveFile( std::string strRelativeFileName )
{
	FileInfo *pInfo = GetFile( strRelativeFileName );
	if ( !pInfo )
		return "";

	return m_strModPath + pInfo->m_strFolderName + pInfo->m_strFileName;
}

int64_t Addon::FileSystem::GetFileSize(std::string strRelativeFileName)
{
	if ( fs_tellmeyoursecrets.GetBool() )
		Msg( "Addon[GetFileSize]: [%s]\n", strRelativeFileName.c_str() );

	FileInfo *pInfo = GetFile( strRelativeFileName );
	if ( !pInfo )
		return -1;

	if ( fs_tellmeyoursecrets.GetInt() > 1 )
		Msg( "Addon[GetFileSize]: Returning [%lli]\n", pInfo->m_nSize );

	return pInfo->m_nSize;
}

Addon::FileInfo *Addon::FileSystem::GetFile( std::string strFileName )
{
	if ( fs_tellmeyoursecrets.GetBool() )
		Msg( "Addon[GetFile]: [%s]\n", strFileName.c_str() );

	std::string strNormalizedPath = strFileName;
	NormalizePath( strNormalizedPath );

	if ( fs_tellmeyoursecrets.GetInt() > 1 )
		Msg( "Addon[GetFile]: Normalized [%s]\n", strNormalizedPath.c_str() );

	std::string folderPath = strNormalizedPath;
	Bootil::String::File::StripFilename( folderPath );

	Folder* folder = GetFolder( folderPath, false );
	if ( !folder || !folder->size() )
		return nullptr;

	std::string fileName = strNormalizedPath;
	Bootil::String::File::ExtractFilename( fileName );

	auto it = folder->find( fileName );
	if ( it == folder->end() )
		return nullptr;
	
	if ( fs_tellmeyoursecrets.GetBool() ) // RaphaelIT7: Custom one
		Msg( "Addon[GetFile]: Found [%s]\n", fileName.c_str() );

	return &it->second;
}

Addon::FileHandle *Addon::FileSystem::GetFileEntry( std::string strFileName )
{
	FileInfo *pInfo = GetFile( strFileName );
	if ( !pInfo )
		return nullptr;

	return new Addon::FileHandle( pInfo );
}

bool Addon::FileSystem::IsOfflineMode()
{
	if ( get->IsDedicatedServer() || !SteamUser() )
		return true;

	return !SteamUser()->BLoggedOn();
}

void Addon::FileSystem::Think()
{
	//Msg( "CAddonFileSystem::Think\n" );
	get->RunSteamCallbacks();
	if ( !m_pCurrentJob && !m_Jobs.empty() )
	{
		m_pCurrentJob = m_Jobs.front();
		m_pCurrentJob->Start();
	}

	if ( m_pCurrentJob )
	{
		m_pCurrentJob->Cycle();

		if ( m_pCurrentJob->Finished() )
		{
			delete m_pCurrentJob;
			m_Jobs.pop_front();
			m_pCurrentJob = nullptr;
		}

		return;
	}

	if ( m_bChanged )
	{
		m_bChanged = false;

		AddJob( new Task::DownloadAddons( true ) );

		if (m_pDownloadNotify)
			m_pDownloadNotify->NotifySubscriptionChanges();
	}
}

void Addon::FileSystem::SetDownloadNotify( IAddonDownloadNotification *pDownloadNotify )
{
	Msg( "CAddonFileSystem::SetDownloadNotify\n" );
	m_pDownloadNotify = pDownloadNotify;
}

IAddonDownloadNotification *Addon::FileSystem::Notify()
{
	Msg( "CAddonFileSystem::Notify\n" );
	return m_pDownloadNotify;
}

bool Addon::FileSystem::IsSubscribed( uint64_t wsid )
{
	Msg( "Addon::FileSystem::IsSubscribed (%llu)\n", wsid );
	for ( IAddonSystem::Information info : m_Addons )
	{
		if ( info.wsid == wsid )
			return true;
	}

	return false;
}

const IAddonSystem::Information* Addon::FileSystem::FindFileOwner( const std::string &strFileName )
{
	Msg( "Addon::FileSystem::FindFileOwner (%s)\n", strFileName.c_str() );
	std::string strNormalizedFileName = strFileName;
	NormalizePath( strNormalizedFileName );

	std::string strFolder = strNormalizedFileName;
	Bootil::String::File::StripFilename( strFolder );
	Folder *pFolder = GetFolder( strFolder, false );
	if ( !pFolder || pFolder->empty() )
		return nullptr;

	for ( auto &entry : *pFolder )
	{
		if ( strNormalizedFileName == ( entry.second.m_strFolderName + entry.second.m_strFileName ) )
		{
			for ( auto &addon : m_Addons )
			{
				if ( addon.wsid == entry.second.m_nWsid )
					return &addon;
			}
		}
	}

	return nullptr;
}

// RaphaelIT7: Note for me dumb dumb. If we give it something, m_Addons makes a copy!
void Addon::FileSystem::AddAddon( const IAddonSystem::Information &info )
{
	Msg( "Addon::FileSystem::AddAddon (%llu)\n", info.wsid );
	m_Addons.push_back( info );
}

void Addon::FileSystem::ClearUnusedGMAs()
{
	Msg( "Addon::FileSystem::ClearUnusedGMAs\n" );
	AddJob( new Addon::Task::ClearUnusedGMAs );
}

Addon::AddonType Addon::FileSystem::GetAddonType(SteamUGCDetails_t details)
{
	const char* tags = details.m_rgchTags;
	if (V_stristr(tags, "dupe,") || V_stristr(tags, ",dupe"))                   return Addon::AddonType::Dupe;         
	if (V_stristr(tags, "save,") || V_stristr(tags, ",save"))                   return Addon::AddonType::Save;         
	if (V_stristr(tags, "demo,") || V_stristr(tags, ",demo"))                   return Addon::AddonType::Demo;         
	if (V_stristr(tags, "addon,") || V_stristr(tags, ",addon"))                 return Addon::AddonType::Addon;        
	if (V_stristr(tags, "servercontent,") || V_stristr(tags, ",servercontent")) return Addon::AddonType::ServerContent;
	return Addon::AddonType::Unknown;
}

std::string Addon::FileSystem::GetAddonFilepath( uint64 wsid, bool bGMAOnly )
{
	Msg( "Addon::FileSystem::GetAddonFilepath (%llu - %s)\n", wsid, bGMAOnly ? "true" : "false" );
	char szFolder[260];
	uint64 size;
	uint32 timestamp;
	if ( !get->SteamUGC()->GetItemInstallInfo( wsid, &size, szFolder, sizeof( szFolder ), &timestamp ) )
		return "";

	std::string strSearchPath = Bootil::String::Format::Print("%s/***.*", szFolder);

	FileFindHandle_t hFindHandle;
	RunCodeAtScopeExit( g_pFullFileSystem->FindClose( hFindHandle ); );
	const char *pszFileName = g_pFullFileSystem->FindFirstEx( strSearchPath.c_str(), nullptr, &hFindHandle );
	while ( pszFileName )
	{
		std::string strFileName = pszFileName;
		std::string ext = Bootil::String::File::GetFileExtension( strFileName );

		bool bValid = false;
		if (bGMAOnly)
		{
			if (ext == "gma")
				bValid = true;
		}
		else
		{
			if (ext == "gma" || ext == "dupe" || ext == "gms" || ext == "dem")
				bValid = true;
		}

		 if ( bValid )
			return Bootil::String::Format::Print( "%s/%s", szFolder, pszFileName );

		pszFileName = g_pFullFileSystem->FindNext( hFindHandle );
	}

	return "";
}

void Addon::FileSystem::UnmountAddon( uint64_t wsid, const char *pszReason )
{
	Msg( "Addon::FileSystem::UnmountAddon (%llu - %s)\n", wsid, pszReason );
}

void Addon::FileSystem::UnmountServerAddons()
{
	Msg( "Addon::FileSystem::UnmountServerAddons\n" );
}

std::string Addon::FileSystem::IsAddonValidPreInstall( SteamUGCDetails_t details )
{
	Msg( "Addon::FileSystem::IsAddonValidPreInstall (%llu)\n", details.m_nPublishedFileId );
	if ( details.m_bBanned )
		return "Addon is banned";

	if ( details.m_eFileType != k_EWorkshopFileTypeCommunity )
		return "Bad workshop file type";

	if ( details.m_rgchTitle[0] == '\0' )
		return "Addon is hidden, banned or doesn't exist";

	if ( details.m_nConsumerAppID != 4000 )
		return "Bad consumer AppID";

	constexpr int LEGACY_CHECK = 0x5E3351E1;
	if ( ( SteamUGC()->GetItemState( details.m_nPublishedFileId ) & k_EItemStateLegacyItem ) != 0 &&
		details.m_rtimeCreated >= LEGACY_CHECK )
	{
		return "Addon too new to use old API";
	}

	return "";
}

// RaphaelIT7:
// The GMod loading screen waits for this to return true
// Else you will wait forever.
bool Addon::FileSystem::AllJobsFinished()
{
	Msg( "Addon::FileSystem::AllJobsFinished\n" );
	return m_pCurrentJob == nullptr && m_Jobs.empty();
}

void Addon::FileSystem::Shutdown()
{
	Msg( "Addon::FileSystem::Shutdown\n" );
}

void Addon::FileSystem::AddJob( Addon::Job::Base* job )
{
	Msg( "Addon::FileSystem::AddJob\n" );
	job->Init(this);
	m_Jobs.push_back(job);
}

const std::list<SteamUGCDetails_t>& Addon::FileSystem::GetSubList() const
{
	Msg( "Addon::FileSystem::GetSubList\n" );
	return m_Subscriptions;
}

void Addon::FileSystem::MountFloatingAddons()
{
	Msg( "Addon::FileSystem::MountFloatingAddons\n" );

	FileFindHandle_t hFindHandle;
	// RaphaelIT7:
	// GMod checks if the gma is inside garrysmod/
	// I dunno why they then just search in MOD...
	const char* pszFile = g_pFullFileSystem->FindFirstEx( "*.gma", "GAME", &hFindHandle );
	while ( pszFile )
	{
		// ToDo
		pszFile = g_pFullFileSystem->FindNext( hFindHandle );
	}

	g_pFullFileSystem->FindClose( hFindHandle );
}

void Addon::FileSystem::AddAddonFromSteamDetails( const SteamUGCDetails_t& details )
{
	for (const auto& info : m_Addons)
		if (info.wsid == details.m_nPublishedFileId)
			return;

	Addon::AddonType type = GetAddonType(details);
	IAddonSystem::Information info = {};
	switch (type)
	{
	case Addon::AddonType::Addon:
	{
		info.title = details.m_rgchTitle;
		info.tags = details.m_rgchTags;
		info.wsid = details.m_nPublishedFileId;
		info.creator = details.m_ulSteamIDOwner;
		info.time_updated = details.m_rtimeUpdated;
		info.size = details.m_nFileSize;
		info.hcontent_file = details.m_hFile;
		info.hcontent_preview = details.m_hPreviewFile;
		info.timeadded = details.m_rtimeAddedToUserList;
		info.canUpdate = false;
		info.downloaded = true;
		break;
	}

	case Addon::AddonType::Dupe:
	case Addon::AddonType::Save:
	case Addon::AddonType::Demo:
	case Addon::AddonType::ServerContent:
		AddUGCFile( details, type );
		return;

	default:
		// Unknown tag: warn / ignore
		Warning( "Addon has unknown type, ignoring (%llu)\n", details.m_nPublishedFileId );
		break;
	}

	std::string strRefuse = IsAddonValidPreInstall( details );
	if ( !strRefuse.empty() )
	{
		Warning( "Error! Refusing to load addon '%s' (%llu)! %s!\n", info.title.c_str(), info.wsid, strRefuse.c_str() );
		AddAddon( info ); // We still wanna show them?
		return;
	}

	if ( SteamUGC()->GetItemState( details.m_nPublishedFileId ) & k_EItemStateInstalled )
	{
		info.downloaded = true;

		char szFolder[260];
		uint64 size;
		uint32 timestamp;
		SteamUGC()->GetItemInstallInfo( details.m_nPublishedFileId, &size, szFolder, sizeof( szFolder ), &timestamp );
		if ( details.m_rtimeUpdated != timestamp )
			info.canUpdate = true;
	}

	std::string strAddonPath = Bootil::String::Format::Print( "cache/workshop/%llu.gma", info.wsid );
	FileHandle_t hFileHandle = g_pFullFileSystem->Open( strAddonPath.c_str(), "rb", "MOD" );

	if ( !hFileHandle )
		hFileHandle = g_pFullFileSystem->Open( strAddonPath.c_str(), "rb", nullptr );

	if ( !hFileHandle )
	{
		// We still add it as the menusystem can show failed to download
		AddAddon(info);
		return;
	}

	info.downloaded = true;

	uint32 nAddonTimestamp = 0;
	g_pFullFileSystem->Seek( hFileHandle, Addon::TimestampOffset, FILESYSTEM_SEEK_HEAD );
	g_pFullFileSystem->Read( &nAddonTimestamp, sizeof(nAddonTimestamp), hFileHandle );
	g_pFullFileSystem->Close( hFileHandle );

	if ( details.m_rtimeUpdated != nAddonTimestamp )
	{
		info.canUpdate = true;
		Msg( "Legacy addon update available! [%s] (US: %u != THEM: %u)\n[%s]\n", details.m_rgchTitle, nAddonTimestamp, details.m_rtimeUpdated, strAddonPath.c_str() );
	}

	AddAddon( info );
}

void Addon::FileSystem::AddUGCFile( SteamUGCDetails_t details, Addon::AddonType type )
{
	IAddonSystem::UGCInfo info = {};
	info.title = details.m_rgchTitle;
	info.file = GetAddonFilepath( details.m_nPublishedFileId, false );
	info.wsid = details.m_nPublishedFileId;
	info.creator = details.m_ulSteamIDOwner;
	info.pubdate = details.m_rtimeUpdated;

	m_UgcAddons.push_back( info );
}

void Addon::FileSystem::OnAddonSubscribed( const SteamUGCDetails_t &details )
{
	Msg( "Addon::FileSystem::OnAddonSubscribed (%llu)\n", details.m_nPublishedFileId );
}

void Addon::FileSystem::AddUnloadedSubscription( uint64_t wsid )
{
	Msg( "Addon::FileSystem::AddUnloadedSubscription (%llu)\n", wsid );
}

void Addon::FileSystem::EnableLoadingUnloadedAddons()
{
}

bool Addon::FileSystem::HasChanges()
{
	Msg( "Addon::FileSystem::HasChanges\n" );
	return m_bChanged;
}

void Addon::FileSystem::MarkChanged()
{
	Msg( "Addon::FileSystem::MarkChanged\n" );
	m_bChanged = true;
}

void Addon::FileSystem::OnAddonDownloaded( const IAddonSystem::Information &info )
{
	Msg( "Addon::FileSystem::AddonDownloaded (%llu)\n", info.wsid );
	MarkChanged();
}

void Addon::FileSystem::OnAddonDownloadFailed( const IAddonSystem::Information &info )
{
	Msg( "Addon::FileSystem::OnAddonDownloadFailed (%llu)\n", info.wsid );
}

void Addon::FileSystem::Load()
{
}

void Addon::FileSystem::FindInAddon( const std::string &pPath, const std::string &pWildcard, std::list<Addon::SearchFile> &results )
{
	for ( MountedAddon &addon : m_MountedAddons )
	{
		if ( addon.m_strTitle != pPath )
			continue;

		FindFirst( pWildcard, results, addon.m_hFileHandle );
	}
}

void Addon::FileSystem::FindFirst( const std::string &pPath, std::list<Addon::SearchFile> &results, FileHandle_t hFileHandle )
{
	std::string path = pPath;
	NormalizePath( path );
	if ( fs_tellmeyoursecrets.GetBool()  )
		Msg( "Addon[FindFirst]: NormalizePath [%s]\n", path.c_str() );

	std::string strDir = path;
	Bootil::String::File::StripFilename( strDir );
	if ( fs_tellmeyoursecrets.GetBool() )
		Msg( "Addon[FindFirst]: strDir [%s]\n", strDir.c_str() );

	for ( auto &[folderName, folder] : m_Folders )
	{
		if ( hFileHandle )
		{
			bool bHasFileFromHandle = false;

			for ( const auto &[fileName, info] : folder )
			{
				if ( info.m_hFileHandle == hFileHandle )
				{
					bHasFileFromHandle = true;
					break;
				}
			}

			if ( !bHasFileFromHandle )
				continue;
		}

		if ( !Bootil::String::Test::Wildcard( path, folderName ) )
			continue;

		if ( folderName.length() < strDir.length() )
			continue;

		std::string relative = folderName.substr( strDir.length() );
		Bootil::String::Util::Trim( relative, "/" );
		if ( relative.empty() )
			continue;

		if ( Bootil::String::Util::Count( relative, '/' ) > 0 )
			continue;

		if ( fs_tellmeyoursecrets.GetBool() )
			Msg( "Addon[FindFirst]: Folder Match [%s]\n", relative.c_str() );

		SearchFile sf;
		sf.m_strFileName = relative;
		sf.m_bFolder = true;
		results.push_back( std::move( sf ) );
	}

	Folder* folder = GetFolder( strDir, false );
	if ( !folder || folder->empty() )
	{
		if ( fs_tellmeyoursecrets.GetBool() )
			Msg( "Addon[FindFirst]: No matching folders or folder contains no files\n" );

		return;
	}

	for ( auto& [name, info] : *folder )
	{
		if ( hFileHandle && info.m_hFileHandle != hFileHandle )
			continue;

		std::string full = strDir + name;
		if ( !Bootil::String::Test::Wildcard( path, full ) )
			continue;

		if ( fs_tellmeyoursecrets.GetBool() )
			Msg( "Addon[FindFirst]: Adding File [%s]\n", full.c_str() );

		SearchFile sf;
		sf.m_strFileName = name;
		sf.m_bFolder = false;
		results.push_back( std::move( sf ) );
	}
}

bool Addon::FileSystem::IsDirectory( std::string strFolderName )
{
	if ( fs_tellmeyoursecrets.GetBool() )
		Msg( "Addon[IsDirectory]: [%s]\n", strFolderName.c_str() );

	NormalizePath( strFolderName );
	if ( fs_tellmeyoursecrets.GetBool() )
		Msg( "Addon[IsDirectory]: Normalized [%s]\n", strFolderName.c_str() );
	
	Folder *pFolder = GetFolder( strFolderName );
	if ( !pFolder || pFolder->empty() )
		return false;

	return true;
}
