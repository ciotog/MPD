// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

#include "UpnpDatabasePlugin.hxx"
#include "Directory.hxx"
#include "Tags.hxx"
#include "lib/upnp/ClientInit.hxx"
#include "lib/upnp/Discovery.hxx"
#include "lib/upnp/ContentDirectoryService.hxx"
#include "db/Interface.hxx"
#include "db/DatabasePlugin.hxx"
#include "db/Selection.hxx"
#include "db/VHelper.hxx"
#include "db/UniqueTags.hxx"
#include "db/DatabaseError.hxx"
#include "db/LightDirectory.hxx"
#include "song/LightSong.hxx"
#include "song/Filter.hxx"
#include "song/TagSongFilter.hxx"
#include "db/Stats.hxx"
#include "tag/Table.hxx"
#include "fs/Traits.hxx"
#include "util/RecursiveMap.hxx"
#include "util/StringSplit.hxx"
#include "config/Block.hxx"

#include <fmt/core.h>

#include <cassert>
#include <string>
#include <utility>

#include <string.h>

static constexpr const char *rootid = "0";
static constexpr std::string_view rootid_sv{rootid};

static std::string_view
AfterRootIdSegment(std::string_view &uri) noexcept
{
	if (uri.size() >= rootid_sv.size() + 2 &&
	    uri.starts_with(rootid_sv) &&
	    uri[rootid_sv.size()] == '/')
		return uri.substr(rootid_sv.size() + 1);
	else
		return {};
}

class UpnpSongData {
protected:
	std::string uri;
	Tag tag;

	template<typename U, typename T>
	UpnpSongData(U &&_uri, T &&_tag) noexcept
		:uri(std::forward<U>(_uri)), tag(std::forward<T>(_tag)) {}
};

class UpnpSong : UpnpSongData, public LightSong {
	std::string real_uri2;

public:
	template<typename U>
	UpnpSong(UPnPDirObject &&object, U &&_uri) noexcept
		:UpnpSongData(std::forward<U>(_uri), std::move(object.tag)),
		 LightSong(UpnpSongData::uri.c_str(), UpnpSongData::tag),
		 real_uri2(std::move(object.url)) {
		real_uri = real_uri2.c_str();
	}
};

class UpnpDatabase : public Database {
	EventLoop &event_loop;
	UpnpClient_Handle handle;
	UPnPDeviceDirectory *discovery;

	const char* iface;

public:
	explicit UpnpDatabase(EventLoop &_event_loop, const ConfigBlock &block) noexcept
		:Database(upnp_db_plugin),
		 event_loop(_event_loop),
		 iface(block.GetBlockValue("interface", nullptr)) {}

	static DatabasePtr Create(EventLoop &main_event_loop,
				  EventLoop &io_event_loop,
				  DatabaseListener &listener,
				  const ConfigBlock &block) noexcept;

	void Open() override;
	void Close() noexcept override;
	[[nodiscard]] const LightSong *GetSong(std::string_view uri_utf8) const override;
	void ReturnSong(const LightSong *song) const noexcept override;

	void Visit(const DatabaseSelection &selection,
		   VisitDirectory visit_directory,
		   VisitSong visit_song,
		   VisitPlaylist visit_playlist) const override;

	[[nodiscard]] RecursiveMap<std::string> CollectUniqueTags(const DatabaseSelection &selection,
								  std::span<const TagType> tag_types) const override;

	[[nodiscard]] DatabaseStats GetStats(const DatabaseSelection &selection) const override;

	[[nodiscard]] std::chrono::system_clock::time_point GetUpdateStamp() const noexcept override {
		return std::chrono::system_clock::time_point::min();
	}

private:
	void VisitServer(const ContentDirectoryService &server,
			 std::string_view uri,
			 const DatabaseSelection &selection,
			 const VisitDirectory& visit_directory,
			 const VisitSong& visit_song,
			 const VisitPlaylist& visit_playlist) const;

	/**
	 * Run an UPnP search according to MPD parameters, and
	 * visit_song the results.
	 */
	void SearchSongs(const ContentDirectoryService &server,
			 const char *objid,
			 const DatabaseSelection &selection,
			 const VisitSong& visit_song) const;

	UPnPDirContent SearchSongs(const ContentDirectoryService &server,
				   const char *objid,
				   const DatabaseSelection &selection) const;

	UPnPDirObject Namei(const ContentDirectoryService &server,
			    std::string_view uri) const;

	/**
	 * Take server and objid, return metadata.
	 */
	UPnPDirObject ReadNode(const ContentDirectoryService &server,
			       const char *objid) const;

	/**
	 * Get the path for an object Id. This works much like pwd,
	 * except easier cause our inodes have a parent id. Not used
	 * any more actually (see comments in SearchSongs).
	 */
	[[nodiscard]] std::string BuildPath(const ContentDirectoryService &server,
			      const UPnPDirObject& dirent) const;
};

DatabasePtr
UpnpDatabase::Create(EventLoop &, EventLoop &io_event_loop,
		     [[maybe_unused]] DatabaseListener &listener,
		     const ConfigBlock &block) noexcept
{
	return std::make_unique<UpnpDatabase>(io_event_loop, block);;
}

void
UpnpDatabase::Open()
{
	handle = UpnpClientGlobalInit(iface);

	discovery = new UPnPDeviceDirectory(event_loop, handle);
	try {
		discovery->Start();
	} catch (...) {
		delete discovery;
		UpnpClientGlobalFinish();
		throw;
	}
}

void
UpnpDatabase::Close() noexcept
{
	delete discovery;
	UpnpClientGlobalFinish();
}

void
UpnpDatabase::ReturnSong(const LightSong *_song) const noexcept
{
	assert(_song != nullptr);

	auto *song = (UpnpSong *)const_cast<LightSong *>(_song);
	delete song;
}

// Get song info by path. We can receive either the id path, or the titles
// one
const LightSong *
UpnpDatabase::GetSong(std::string_view uri) const
{
	auto [server_name, uri_in_server] = Split(uri, '/');
	if (server_name.empty() || uri_in_server.empty())
		throw DatabaseError(DatabaseErrorCode::NOT_FOUND,
				    "No such song");

	auto server = discovery->GetServer(server_name);

	UPnPDirObject dirent;
	if (const auto id = AfterRootIdSegment(uri_in_server);
	    id.data() == nullptr) {
		dirent = Namei(server, uri_in_server);
	} else {
		dirent = ReadNode(server, std::string{id}.c_str());
	}

	return new UpnpSong(std::move(dirent), uri);
}

/**
 * Double-quote a string, adding internal backslash escaping.
 */
static void
dquote(std::string &out, const char *in) noexcept
{
	out.push_back('"');

	for (; *in != 0; ++in) {
		switch(*in) {
		case '\\':
		case '"':
			out.push_back('\\');
			break;
		}

		out.push_back(*in);
	}

	out.push_back('"');
}

// Run an UPnP search, according to MPD parameters. Return results as
// UPnP items
UPnPDirContent
UpnpDatabase::SearchSongs(const ContentDirectoryService &server,
			  const char *objid,
			  const DatabaseSelection &selection) const
{
	const SongFilter *filter = selection.filter;
	if (selection.filter == nullptr)
		return {};

	const auto searchcaps = server.getSearchCapabilities(handle);
	if (searchcaps.empty())
		return {};

	std::string cond;
	for (const auto &item : filter->GetItems()) {
		if (auto t = dynamic_cast<const TagSongFilter *>(item.get())) {
			auto tag = t->GetTagType();
			if (tag == TAG_NUM_OF_ITEM_TYPES) {
				if (!cond.empty()) {
					cond += " and ";
				}
				cond += '(';
				bool first(true);
				for (const auto& cap : searchcaps) {
					if (first)
						first = false;
					else
						cond += " or ";
					cond += cap;
					if (t->GetFoldCase()) {
						cond += " contains ";
					} else {
						cond += " = ";
					}
					dquote(cond, t->GetValue().c_str());
				}
				cond += ')';
				continue;
			}

			if (tag == TAG_ALBUM_ARTIST)
				tag = TAG_ARTIST;

			const char *name = tag_table_lookup(upnp_tags, tag);
			if (name == nullptr)
				continue;

			if (!cond.empty()) {
				cond += " and ";
			}
			cond += name;

			/* FoldCase doubles up as contains/equal
			   switch. UpNP search is supposed to be
			   case-insensitive, but at least some servers
			   have the same convention as mpd (e.g.:
			   minidlna) */
			if (t->GetFoldCase()) {
				cond += " contains ";
			} else {
				cond += " = ";
			}
			dquote(cond, t->GetValue().c_str());
		}

		// TODO: support other ISongFilter implementations
	}

	return server.search(handle, objid, cond.c_str());
}

static void
visitSong(const UPnPDirObject &meta, const char *path,
	  const DatabaseSelection &selection,
	  const VisitSong& visit_song)
{
	if (!visit_song)
		return;

	LightSong song(path, meta.tag);
	song.real_uri = meta.url.c_str();

	if (selection.Match(song))
		visit_song(song);
}

/**
 * Build synthetic path based on object id for search results. The use
 * of "rootid" is arbitrary, any name that is not likely to be a top
 * directory name would fit.
 */
static std::string
songPath(const std::string_view servername,
	 const std::string_view objid) noexcept
{
	return fmt::format("{}/{}/{}", servername, rootid_sv, objid);
}

void
UpnpDatabase::SearchSongs(const ContentDirectoryService &server,
			  const char *objid,
			  const DatabaseSelection &selection,
			  const VisitSong& visit_song) const
{
	if (!visit_song)
		return;

	const auto content = SearchSongs(server, objid, selection);
	for (auto &dirent : content.objects) {
		if (dirent.type != UPnPDirObject::Type::ITEM ||
		    dirent.item_class != UPnPDirObject::ItemClass::MUSIC)
			continue;

		// We get song ids as the result of the UPnP search. But our
		// client expects paths (e.g. we get 1$4$3788 from minidlna,
		// but we need to translate to /Music/All_Music/Satisfaction).
		// We can do this in two ways:
		//  - Rebuild a normal path using BuildPath() which is a kind of pwd
		//  - Build a bogus path based on the song id.
		// The first method is nice because the returned paths are pretty, but
		// it has two big problems:
		//  - The song paths are ambiguous: e.g. minidlna returns all search
		//    results as being from the "All Music" directory, which can
		//    contain several songs with the same title (but different objids)
		//  - The performance of BuildPath() is atrocious on very big
		//    directories, even causing timeouts in clients. And of
		//    course, 'All Music' is very big.
		// So we return synthetic and ugly paths based on the object id,
		// which we later have to detect.
		const std::string path = songPath(server.GetFriendlyName(),
						  dirent.id);
		visitSong(dirent, path.c_str(),
			  selection, visit_song);
	}
}

UPnPDirObject
UpnpDatabase::ReadNode(const ContentDirectoryService &server,
		       const char *objid) const
{
	auto dirbuf = server.getMetadata(handle, objid);
	if (dirbuf.objects.size() != 1)
		throw std::runtime_error("Bad resource");

	return std::move(dirbuf.objects.front());
}

std::string
UpnpDatabase::BuildPath(const ContentDirectoryService &server,
			const UPnPDirObject& idirent) const
{
	const char *pid = idirent.id.c_str();
	std::string path;
	while (strcmp(pid, rootid) != 0) {
		auto dirent = ReadNode(server, pid);
		pid = dirent.parent_id.c_str();

		if (path.empty())
			path = dirent.name;
		else
			path = PathTraitsUTF8::Build(dirent.name, path);
	}

	return PathTraitsUTF8::Build(server.GetFriendlyName(), path);
}

// Take server and internal title pathname and return objid and metadata.
UPnPDirObject
UpnpDatabase::Namei(const ContentDirectoryService &server,
		    std::string_view uri) const
{
	if (uri.empty())
		// looking for root info
		return ReadNode(server, rootid);

	std::string objid(rootid_sv);

	// Walk the path elements, read each directory and try to find the next one
	while (true) {
		auto dirbuf = server.readDir(handle, objid.c_str());

		const auto [name, rest] = Split(uri, '/');

		// Look for the name in the sub-container list
		UPnPDirObject *child = dirbuf.FindObject(name);
		if (child == nullptr)
			throw DatabaseError(DatabaseErrorCode::NOT_FOUND,
					    "No such object");

		uri = rest;
		if (uri.empty())
			return std::move(*child);

		if (child->type != UPnPDirObject::Type::CONTAINER)
			throw DatabaseError(DatabaseErrorCode::NOT_FOUND,
					    "Not a container");

		objid = std::move(child->id);
	}
}

static void
VisitItem(const UPnPDirObject &object, const char *uri,
	  const DatabaseSelection &selection,
	  const VisitSong& visit_song, const VisitPlaylist& visit_playlist)
{
	assert(object.type == UPnPDirObject::Type::ITEM);

	switch (object.item_class) {
	case UPnPDirObject::ItemClass::MUSIC:
		visitSong(object, uri, selection, visit_song);
		break;

	case UPnPDirObject::ItemClass::PLAYLIST:
		if (visit_playlist) {
			/* Note: I've yet to see a
			   playlist item (playlists
			   seem to be usually handled
			   as containers, so I'll
			   decide what to do when I
			   see one... */
		}

		break;

	case UPnPDirObject::ItemClass::UNKNOWN:
		break;
	}
}

static void
VisitObject(const UPnPDirObject &object, const char *uri,
	    const DatabaseSelection &selection,
	    const VisitDirectory& visit_directory,
	    const VisitSong& visit_song,
	    const VisitPlaylist& visit_playlist)
{
	switch (object.type) {
	case UPnPDirObject::Type::UNKNOWN:
		std::unreachable();

	case UPnPDirObject::Type::CONTAINER:
		if (visit_directory)
			visit_directory(LightDirectory(uri,
						       std::chrono::system_clock::time_point::min()));
		break;

	case UPnPDirObject::Type::ITEM:
		VisitItem(object, uri, selection,
			  visit_song, visit_playlist);
		break;
	}
}

// vpath is a parsed and writeable version of selection.uri. There is
// really just one path parameter.
void
UpnpDatabase::VisitServer(const ContentDirectoryService &server,
			  std::string_view uri,
			  const DatabaseSelection &selection,
			  const VisitDirectory& visit_directory,
			  const VisitSong& visit_song,
			  const VisitPlaylist& visit_playlist) const
{
	/* If the path begins with rootid, we know that this is a
	   song, not a directory (because that's how we set things
	   up). Just visit it. Note that the choice of rootid is
	   arbitrary, any value not likely to be the name of a top
	   directory would be ok. */
	/* !Note: this *can't* be handled by Namei further down,
	   because the path is not valid for traversal. Besides, it's
	   just faster to access the target node directly */
	if (uri == rootid_sv)
		return;

	if (const auto id = AfterRootIdSegment(uri); id.data() != nullptr) {
		if (visit_song) {
			auto dirent = ReadNode(server, std::string{id}.c_str());

			if (dirent.type != UPnPDirObject::Type::ITEM ||
			    dirent.item_class != UPnPDirObject::ItemClass::MUSIC)
				throw DatabaseError(DatabaseErrorCode::NOT_FOUND,
						    "Not found");

			std::string path = songPath(server.GetFriendlyName(),
						    dirent.id);
			visitSong(dirent, path.c_str(),
				  selection, visit_song);
		}

		return;
	}

	// Translate the target path into an object id and the associated metadata.
	const auto tdirent = Namei(server, uri);

	/* If recursive is set, this is a search... No use sending it
	   if the filter is empty. In this case, we implement limited
	   recursion (1-deep) here, which will handle the "add dir"
	   case. */
	if (selection.recursive && selection.filter) {
		SearchSongs(server, tdirent.id.c_str(), selection, visit_song);
		return;
	}

	const char *const base_uri = selection.uri.empty()
		? server.GetFriendlyName().c_str()
		: selection.uri.c_str();

	if (tdirent.type == UPnPDirObject::Type::ITEM) {
		VisitItem(tdirent, base_uri,
			  selection,
			  visit_song, visit_playlist);
		return;
	}

	/* Target was a a container. Visit it. We could read slices
	   and loop here, but it's not useful as mpd will only return
	   data to the client when we're done anyway. */
	const auto contents = server.readDir(handle, tdirent.id.c_str());
	for (const auto &dirent : contents.objects) {
		const std::string child_uri = PathTraitsUTF8::Build(base_uri,
								    dirent.name.c_str());
		VisitObject(dirent, child_uri.c_str(),
			    selection,
			    visit_directory,
			    visit_song, visit_playlist);
	}
}

[[gnu::const]]
static DatabaseSelection
CheckSelection(DatabaseSelection selection) noexcept
{
	selection.uri.clear();
	selection.filter = nullptr;
	return selection;
}

// Deal with the possibly multiple servers, call VisitServer if needed.
void
UpnpDatabase::Visit(const DatabaseSelection &selection,
		    VisitDirectory visit_directory,
		    VisitSong visit_song,
		    VisitPlaylist visit_playlist) const
{
	DatabaseVisitorHelper helper(CheckSelection(selection), visit_song);

	if (selection.uri.empty()) {
		for (const auto &server : discovery->GetDirectories()) {
			if (visit_directory) {
				const LightDirectory d(server.GetFriendlyName().c_str(),
						       std::chrono::system_clock::time_point::min());
				visit_directory(d);
			}

			if (selection.recursive)
				VisitServer(server, {}, selection,
					    visit_directory, visit_song,
					    visit_playlist);
		}

		helper.Commit();
		return;
	}

	// We do have a path: the first element selects the server
	const auto [server_name, uri_in_server] = Split(std::string_view{selection.uri}, '/');
	auto server = discovery->GetServer(server_name);

	VisitServer(server, uri_in_server, selection,
		    visit_directory, visit_song, visit_playlist);
	helper.Commit();
}

RecursiveMap<std::string>
UpnpDatabase::CollectUniqueTags(const DatabaseSelection &selection,
				std::span<const TagType> tag_types) const
{
	return ::CollectUniqueTags(*this, selection, tag_types);
}

DatabaseStats
UpnpDatabase::GetStats(const DatabaseSelection &) const
{
	/* Note: this gets called before the daemonizing so we can't
	   reallyopen this would be a problem if we had real stats */
	DatabaseStats stats;
	stats.Clear();
	return stats;
}

const DatabasePlugin upnp_db_plugin = {
	"upnp",
	0,
	UpnpDatabase::Create,
};
