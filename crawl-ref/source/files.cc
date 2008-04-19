/*
 *  File:       files.cc
 *  Summary:    Functions used to save and load levels/games.
 *  Written by: Linley Henzell and Alexey Guzeev
 *
 *  Modified for Crawl Reference by $Author$ on $Date$
 *
 *  Change History (most recent first):
 *
 *   <7>   19 June 2000  GDL    Change handle to FILE *
 *   <6>   11/14/99      cdl    Don't let player ghosts follow you up/down
 *   <5>    7/13/99      BWR    Monsters now regenerate hps off level &
 ghosts teleport
 *   <4>    6/13/99      BWR    Added tmp file pairs to save file.
 *   <3>    6/11/99      DML    Replaced temp file deletion code.
 *
 *   <2>    5/12/99      BWR    Multiuser system support,
 *                                        including appending UID to
 *                                        name, and compressed saves
 *                                        in the SAVE_DIR_PATH directory
 *
 *   <1>   --/--/--      LRH    Created
 */

#include "AppHdr.h"
#include "files.h"
#include "version.h"

#include <string.h>
#include <string>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#include <algorithm>
#include <functional>

#ifdef DOS
#include <conio.h>
#include <file.h>
#endif

#ifdef UNIX
#include <fcntl.h>
#include <unistd.h>
#endif

#ifdef __MINGW32__
#include <io.h>
#endif

#include <sys/types.h>
#include <sys/stat.h>

#include "externs.h"

#include "chardump.h"
#include "cloud.h"
#include "clua.h"
#include "debug.h"
#include "directn.h"
#include "dungeon.h"
#include "effects.h"
#include "ghost.h"
#include "initfile.h"
#include "itemname.h"
#include "itemprop.h"
#include "items.h"
#include "Kills.h"
#include "libutil.h"
#include "mapmark.h"
#include "message.h"
#include "misc.h"
#include "monstuff.h"
#include "mon-util.h"
#include "mstuff2.h"
#include "mtransit.h"
#include "notes.h"
#include "output.h"
#include "overmap.h"
#include "place.h"
#include "player.h"
#include "randart.h"
#include "skills2.h"
#include "stash.h"
#include "state.h"
#include "stuff.h"
#include "tags.h"
#include "tiles.h"
#include "terrain.h"
#include "travel.h"
#include "tutorial.h"
#include "view.h"
#include "xom.h"

#if _MSC_VER
#include <direct.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#endif

#ifndef HAVE_STAT
#if defined(UNIX) || defined(__MINGW32__) || defined(DOS)
#define HAVE_STAT
#endif
#endif

void save_level(int level_saved, level_area_type lt,
                branch_type where_were_you);

#define GHOST_MINOR_VERSION 1
#define LEVEL_MINOR_VERSION 1

// 1: starting version
// 2: append piety_hysteresis to TAG_YOU
// 3: add quiver info.
#define YOU_MINOR_VERSION   3

const short GHOST_SIGNATURE = short( 0xDC55 );

static void redraw_all(void)
{
    you.redraw_hit_points = 1;
    you.redraw_magic_points = 1;
    you.redraw_strength = 1;
    you.redraw_intelligence = 1;
    you.redraw_dexterity = 1;
    you.redraw_armour_class = 1;
    you.redraw_evasion = 1;
    you.redraw_experience = 1;
    you.redraw_gold = 1;

    you.redraw_status_flags =
        REDRAW_LINE_1_MASK | REDRAW_LINE_2_MASK | REDRAW_LINE_3_MASK;
}

static bool determine_version( FILE *restoreFile,
                               char &majorVersion, char &minorVersion );

static void restore_version( FILE *restoreFile,
                             char majorVersion, char minorVersion );

static bool determine_level_version( FILE *levelFile,
                                     char &majorVersion, char &minorVersion );

static void restore_level_version( FILE *levelFile,
                                   char majorVersion, char minorVersion );

static bool determine_ghost_version( FILE *ghostFile,
                                     char &majorVersion, char &minorVersion );

static void restore_ghost_version( FILE *ghostFile,
                                   char majorVersion, char minorVersion );

static void restore_tagged_file( FILE *restoreFile, int fileType,
                                 char minorVersion );

static void load_ghost();

static std::string uid_as_string()
{
#ifdef MULTIUSER
    char struid[20];
    snprintf( struid, sizeof struid, "-%d", static_cast<int>(getuid()) );
    return std::string(struid);
#else
    return std::string();
#endif
}

static bool is_uid_file(const std::string &name, const std::string &ext)
{
    std::string save_suffix = get_savedir_filename("", "", "");
    save_suffix += ext;
#ifdef DOS
    // Grumble grumble. Hang all retarded operating systems.
    uppercase(save_suffix);
#endif

    save_suffix = save_suffix.substr(Options.save_dir.length());

    std::string::size_type suffix_pos = name.find(save_suffix);
    return (suffix_pos != std::string::npos
            && suffix_pos == name.length() - save_suffix.length()
            && suffix_pos != 0);
}

bool is_save_file_name(const std::string &name)
{
    return is_uid_file(name, ".sav");
}

#ifdef LOAD_UNPACKAGE_CMD
bool is_packed_save(const std::string &name)
{
    return is_uid_file(name, PACKAGE_SUFFIX);
}
#endif

// Return the save_info from the save.
player_save_info read_character_info(const std::string &savefile)
{
    player_save_info fromfile;
    FILE *charf = fopen(savefile.c_str(), "rb");
    if (!charf)
        return fromfile;

    char majorVersion = 0;
    char minorVersion = 0;

    if (determine_version(charf, majorVersion, minorVersion)
        && majorVersion == SAVE_MAJOR_VERSION)
    {
        // backup before we clobber "you"
        const player backup(you);

        restore_tagged_file(charf, TAGTYPE_PLAYER_NAME, minorVersion);

        fromfile = you;
        you.copy_from(backup);
    }

    fclose(charf);
    return fromfile;
}

static bool _is_good_filename(const std::string &s)
{
    return (s != "." && s != "..");
}

// Returns the names of all files in the given directory. Note that the
// filenames returned are relative to the directory.
std::vector<std::string> get_dir_files(const std::string &dirname)
{
    std::vector<std::string> files;

#ifdef _MSC_VER
    WIN32_FIND_DATA lData;
    HANDLE hFind = FindFirstFile(dirname.c_str(), &lData);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        if (_is_good_filename(lData.cFileName))
            files.push_back(lData.cFileName);
        while (FindNextFile(hFind, &lData))
            files.push_back(lData.cFileName);
        FindClose(hFind);
    }
#else // non-MS VC++ compilers

    DIR *dir = opendir(dirname.c_str());
    if (!dir)
        return (files);

    while (dirent *entry = readdir(dir))
    {
        std::string name = entry->d_name;
        if (name == "." || name == "..")
            continue;

        files.push_back(name);
    }
    closedir(dir);
#endif

    return (files);
}

std::string get_parent_directory(const std::string &filename)
{
    std::string::size_type pos = filename.rfind(FILE_SEPARATOR);
    if (pos != std::string::npos)
        return filename.substr(0, pos + 1);
#ifdef ALT_FILE_SEPARATOR
    pos = filename.rfind(ALT_FILE_SEPARATOR);
    if (pos != std::string::npos)
        return filename.substr(0, pos + 1);
#endif
    return ("");
}

std::string get_base_filename(const std::string &filename)
{
    std::string::size_type pos = filename.rfind(FILE_SEPARATOR);
    if (pos != std::string::npos)
        return filename.substr(pos + 1);
#ifdef ALT_FILE_SEPARATOR
    pos = filename.rfind(ALT_FILE_SEPARATOR);
    if (pos != std::string::npos)
        return filename.substr(pos + 1);
#endif
    return (filename);
}

std::string change_file_extension(const std::string &filename,
                                  const std::string &ext)
{
    const std::string::size_type pos = filename.rfind('.');
    return ((pos == std::string::npos? filename : filename.substr(0, pos))
            + ext);
}

time_t file_modtime(const std::string &file)
{
    struct stat filestat;
    if (stat(file.c_str(), &filestat))
        return (0);

    return (filestat.st_mtime);
}

// Returns true if file a is newer than file b.
bool is_newer(const std::string &a, const std::string &b)
{
    return (file_modtime(a) > file_modtime(b));
}

void check_newer(const std::string &target,
                 const std::string &dependency,
                 void (*action)())
{
    if (is_newer(dependency, target))
        action();
}

static bool file_exists(const std::string &name)
{
#ifdef HAVE_STAT
    struct stat st;
    const int err = ::stat(name.c_str(), &st);
    return (!err);
#else
    FILE *f = fopen(name.c_str(), "r");
    const bool exists = !!f;
    if (f)
        fclose(f);
    return (exists);
#endif
}

// Low-tech existence check.
static bool dir_exists(const std::string &dir)
{
#ifdef _MSC_VER
    DWORD lAttr = GetFileAttributes(dir.c_str());
    return (lAttr != INVALID_FILE_ATTRIBUTES 
            && (lAttr & FILE_ATTRIBUTE_DIRECTORY));
#else
    DIR *d = opendir(dir.c_str());
    const bool exists = !!d;
    if (d)
        closedir(d);

    return (exists);
#endif
}

static int create_directory(const char *dir)
{
#if defined(MULTIUSER)
    return mkdir(dir, SHARED_FILES_CHMOD_PUBLIC | 0111);
#elif defined(DOS)
    return mkdir(dir, 0755);
#elif defined(_MSC_VER)
    return _mkdir(dir);
#else
    return mkdir(dir);
#endif
}

static bool create_dirs(const std::string &dir)
{
    std::string sep = " ";
    sep[0] = FILE_SEPARATOR;
    std::vector<std::string> segments =
        split_string(
                sep.c_str(),
                dir,
                false,
                false);

    std::string path;
    for (int i = 0, size = segments.size(); i < size; ++i)
    {
        path += segments[i];

        // Handle absolute paths correctly.
        if (i == 0 && dir.size() && dir[0] == FILE_SEPARATOR)
            path = FILE_SEPARATOR + path;

        if (!dir_exists(path) && create_directory(path.c_str()))
            return (false);

        path += FILE_SEPARATOR;
    }
    return (true);
}

std::string datafile_path(std::string basename,
                          bool croak_on_fail,
                          bool test_base_path)
{
#if FILE_SEPARATOR != '/'
    basename = replace_all_of(basename, "/", std::string(1, FILE_SEPARATOR));
#endif

    if (test_base_path && file_exists(basename))
        return (basename);

    std::string cdir = !SysEnv.crawl_dir.empty()? SysEnv.crawl_dir : "";

    const std::string rawbases[] = {
#ifdef DATA_DIR_PATH
        DATA_DIR_PATH,
#else
        cdir,
#endif
    };

    const std::string prefixes[] = {
        std::string("dat") + FILE_SEPARATOR,
        std::string("docs") + FILE_SEPARATOR,
#ifndef DATA_DIR_PATH
        std::string("..") + FILE_SEPARATOR + "docs" + FILE_SEPARATOR,
        std::string("..") + FILE_SEPARATOR + "dat" + FILE_SEPARATOR,
        std::string("..") + FILE_SEPARATOR,
#endif
        std::string(".") + FILE_SEPARATOR,
        "",
    };

    std::vector<std::string> bases;
    for (unsigned i = 0; i < sizeof(rawbases) / sizeof(*rawbases); ++i)
    {
        std::string base = rawbases[i];
        if (base.empty())
            continue;

        if (base[base.length() - 1] != FILE_SEPARATOR)
            base += FILE_SEPARATOR;
        bases.push_back(base);
    }

#ifndef DATA_DIR_PATH
    if (!SysEnv.crawl_base.empty())
        bases.push_back(SysEnv.crawl_base);
    bases.push_back("");
#endif

    for (unsigned b = 0, size = bases.size(); b < size; ++b)
    {
        for (unsigned p = 0; p < sizeof(prefixes) / sizeof(*prefixes); ++p)
        {
            std::string name = bases[b] + prefixes[p] + basename;
            if (file_exists(name))
                return (name);
        }
    }

    // Die horribly.
    if (croak_on_fail)
        end(1, false, "Cannot find data file '%s' anywhere, aborting\n",
            basename.c_str());

    return ("");
}

bool check_dir(const std::string &whatdir, std::string &dir, bool silent)
{
    if (dir.empty())
        return (true);

    const std::string sep(1, FILE_SEPARATOR);

    dir = replace_all_of(dir, "/", sep);
    dir = replace_all_of(dir, "\\", sep);

    // Suffix the separator if necessary
    if (dir[dir.length() - 1] != FILE_SEPARATOR)
        dir += FILE_SEPARATOR;

    if (!dir_exists(dir) && !create_dirs(dir))
    {
        if (!silent)
            fprintf(stderr, "%s \"%s\" does not exist "
                    "and I can't create it.\n",
                    whatdir.c_str(), dir.c_str());
        return (false);
    }

    return (true);
}

// Given a simple (relative) name of a save file, returns the full path of
// the file in the Crawl saves directory. You can use path segments in
// shortpath (separated by /) and get_savedir_path will canonicalise them
// to the platform's native file separator.
std::string get_savedir_path(const std::string &shortpath)
{
    const std::string file = Options.save_dir + shortpath;
#if FILE_SEPARATOR != '/'
    return (replace_all(file, "/", std::string(1, FILE_SEPARATOR)));
#else
    return (file);
#endif
}

/*
 * Returns a list of the names of characters that are already saved for the
 * current user.
 */

std::vector<player_save_info> find_saved_characters()
{
    std::vector<player_save_info> chars;
#ifndef DISABLE_SAVEGAME_LISTS
    std::string searchpath = Options.save_dir;

    if (searchpath.empty())
        searchpath = ".";

    std::vector<std::string> allfiles = get_dir_files(searchpath);
    for (unsigned int i = 0; i < allfiles.size(); ++i)
    {
        std::string filename = allfiles[i];
#ifdef LOAD_UNPACKAGE_CMD
        if (!is_packed_save(filename))
            continue;

        std::string basename =
            filename.substr(
                    0,
                    filename.length() - strlen(PACKAGE_SUFFIX));

        const std::string zipname = get_savedir_path(basename);

        // This is the filename we actually read ourselves.
        filename = basename + ".sav";

        const std::string dir = get_savedir();

        char cmd_buff[1024];
        snprintf( cmd_buff, sizeof(cmd_buff), UNPACK_SPECIFIC_FILE_CMD,
                  zipname.c_str(),
                  dir.c_str(),
                  filename.c_str() );

        if (system(cmd_buff) != 0)
            continue;
#endif
        if (is_save_file_name(filename))
        {
            player_save_info p=read_character_info(get_savedir_path(filename));
            if (!p.name.empty())
                chars.push_back(p);
        }

#ifdef LOAD_UNPACKAGE_CMD
        // If we unpacked the .sav file, throw it away now.
        unlink( get_savedir_path(filename).c_str() );
#endif
    }

    std::sort(chars.rbegin(), chars.rend());
#endif // !DISABLE_SAVEGAME_LISTS
    return (chars);
}

std::string get_savedir()
{
    const std::string &dir = Options.save_dir;
    return (dir.empty()? "." : dir);
}

std::string get_savedir_filename(const std::string &prefix,
                                 const std::string &suffix,
                                 const std::string &extension,
                                 bool suppress_uid)
{
    std::string result = Options.save_dir;

    // Shorten string as appropriate
    result += strip_filename_unsafe_chars(prefix).substr(0, kFileNameLen);

    // Technically we should shorten the string first.  But if
    // MULTIUSER is set we'll have long filenames anyway. Caveat
    // emptor.
    if ( !suppress_uid )
        result += uid_as_string();

    result += suffix;

    if (!extension.empty())
    {
        result += '.';
        result += extension;
    }

#ifdef DOS
    uppercase(result);
#endif
    return result;
}

std::string get_prefs_filename()
{
#ifdef DGL_STARTUP_PREFS_BY_NAME
    return get_savedir_filename("start-" + Options.player_name + "-",
                                "ns", "prf", true);
#else
    return get_savedir_filename("start", "ns", "prf");
#endif
}

static std::string get_level_suffix(int level, branch_type where,
                                    level_area_type lt)
{
    switch (lt)
    {
    default:
    case LEVEL_DUNGEON:
        return (make_stringf("%02d%c", subdungeon_depth(where, level),
                             where + 'a'));
    case LEVEL_LABYRINTH:
        return ("lab");
    case LEVEL_ABYSS:
        return ("abs");
    case LEVEL_PANDEMONIUM:
        return ("pan");
    case LEVEL_PORTAL_VAULT:
        return ("ptl");
    }
}

std::string make_filename( const char *prefix, int level, branch_type where,
                           level_area_type ltype, bool isGhost )
{
    return get_savedir_filename( prefix, "",
                                 get_level_suffix(level, where, ltype),
                                 isGhost );
}

static void write_version( FILE *dataFile, int majorVersion, int minorVersion,
                           bool extended_version )
{
    // write version
    writer outf(dataFile);

    marshallByte(outf, majorVersion);
    marshallByte(outf, minorVersion);

    // extended_version just pads the version out to four 32-bit words.
    // This makes the bones file compatible with Hearse with no extra
    // munging needed.
    if (extended_version)
    {
        // Use a single signature 16-bit word to indicate that this is
        // Stone Soup and to disambiguate this (unmunged) bones file
        // from the munged bones files offered by the old Crawl-aware
        // hearse.pl. Crawl-aware hearse.pl will prefix the bones file
        // with the first 16-bits of the Crawl version, and the following
        // 7 16-bit words set to 0.
        marshallShort(outf, GHOST_SIGNATURE);

        // Write the three remaining 32-bit words of padding.
        for (int i = 0; i < 3; ++i)
            marshallLong(outf, 0);
    }
}

static void write_tagged_file( FILE *dataFile, char majorVersion,
                               char minorVersion, int fileType,
                               bool extended_version = false )
{
    // find all relevant tags
    char tags[NUM_TAGS];
    tag_set_expected(tags, fileType);

    write_version( dataFile, majorVersion, minorVersion, extended_version );

    // all other tags
    for (int i = 1; i < NUM_TAGS; i++)
    {
        if (tags[i] == 1)
            tag_write((tag_type)i, dataFile);
    }
}

bool travel_load_map( branch_type branch, int absdepth )
{
    // Try to open level savefile.
    FILE *levelFile = fopen(make_filename(you.your_name, absdepth, branch,
                                          LEVEL_DUNGEON, false).c_str(), "rb");
    if (!levelFile)
        return false;

    // BEGIN -- must load the old level : pre-load tasks

    // LOAD various tags
    char majorVersion;
    char minorVersion;

    if (!determine_level_version( levelFile, majorVersion, minorVersion )
            || majorVersion != SAVE_MAJOR_VERSION)
    {
        fclose(levelFile);
        return false;
    }

    tag_read(levelFile, minorVersion);

    fclose( levelFile );

    return true;
}

static void sanity_test_monster_inventory()
{
    // Sanity forcing of monster inventory items (required?)
    for (int i = 0; i < MAX_MONSTERS; i++)
    {
        if (menv[i].type == -1)
            continue;

        for (int j = 0; j < NUM_MONSTER_SLOTS; j++)
        {
            if (menv[i].inv[j] == NON_ITEM)
                continue;

            // items carried by monsters shouldn't be linked
            if (mitm[menv[i].inv[j]].link != NON_ITEM)
                mitm[menv[i].inv[j]].link = NON_ITEM;
        }
    }
}

static void place_player_on_stair(branch_type old_branch,
                                  int stair_taken)
{
    bool find_first = true;

    // Order is important here:
    if (you.level_type == LEVEL_DUNGEON
        && old_branch == BRANCH_VESTIBULE_OF_HELL
        && stair_taken == DNGN_STONE_STAIRS_UP_I)
    {
        // leaving hell - look for entry portal first
        stair_taken = DNGN_ENTER_HELL;
        find_first = false;
    }
    else if (stair_taken == DNGN_EXIT_PANDEMONIUM)
    {
        stair_taken = DNGN_ENTER_PANDEMONIUM;
        find_first = false;
    }
    else if (stair_taken == DNGN_EXIT_ABYSS)
    {
        stair_taken = DNGN_ENTER_ABYSS;
        find_first = false;
    }
    else if (stair_taken == DNGN_ENTER_HELL)
    {
        // the vestibule and labyrith always start from this stair
        stair_taken = DNGN_STONE_STAIRS_UP_I;
    }
    else if (stair_taken >= DNGN_STONE_STAIRS_DOWN_I
             && stair_taken <= DNGN_ESCAPE_HATCH_DOWN)
    {
        // look for coresponding up stair
        stair_taken += (DNGN_STONE_STAIRS_UP_I - DNGN_STONE_STAIRS_DOWN_I);
    }
    else if (stair_taken >= DNGN_STONE_STAIRS_UP_I
             && stair_taken <= DNGN_ESCAPE_HATCH_UP)
    {
        // look for coresponding down stair
        stair_taken += (DNGN_STONE_STAIRS_DOWN_I - DNGN_STONE_STAIRS_UP_I);
    }
    else if (stair_taken >= DNGN_RETURN_FROM_FIRST_BRANCH
             && stair_taken < 150) // 20 slots reserved
    {
        // find entry point to subdungeon when leaving
        stair_taken += (DNGN_ENTER_FIRST_BRANCH
                        - DNGN_RETURN_FROM_FIRST_BRANCH);
    }
    else if (stair_taken >= DNGN_ENTER_FIRST_BRANCH
             && stair_taken < DNGN_RETURN_FROM_FIRST_BRANCH)
    {
        // find exit staircase from subdungeon when entering
        stair_taken += (DNGN_RETURN_FROM_FIRST_BRANCH
                        - DNGN_ENTER_FIRST_BRANCH);
    }
    else if (stair_taken >= DNGN_ENTER_DIS
             && stair_taken <= DNGN_TRANSIT_PANDEMONIUM)
    {
        // when entering a hell or pandemonium
        stair_taken = DNGN_STONE_STAIRS_UP_I;
    }
    else if (stair_taken == DNGN_ENTER_PORTAL_VAULT)
    {
        stair_taken = DNGN_STONE_ARCH;
    }
    else if (stair_taken == DNGN_EXIT_PORTAL_VAULT)
    {
        stair_taken = DNGN_ESCAPE_HATCH_DOWN;
    }
    else if (stair_taken == DNGN_ENTER_LABYRINTH)
    {
        // dgn_find_nearby_stair uses special logic for labyrinths.
        stair_taken = DNGN_ENTER_LABYRINTH;
    }
    else // Note: stair_taken can equal things like DNGN_FLOOR
    {
        // just find a nice empty square
        stair_taken = DNGN_FLOOR;
        find_first = false;
    }

    const coord_def where_to_go =
        dgn_find_nearby_stair(static_cast<dungeon_feature_type>(stair_taken),
                              you.pos(), find_first);
    you.moveto(where_to_go);
}

static void close_level_gates()
{
    for ( int i = 0; i < GXM; ++i )
    {
        for ( int j = 0; j < GYM; ++j )
        {
            if (you.char_direction == GDT_ASCENDING
                && you.level_type != LEVEL_PANDEMONIUM)
            {
                if (grid_sealable_portal(grd[i][j]))
                {
                    grd[i][j] = DNGN_STONE_ARCH;
                    env.markers.remove_markers_at(coord_def(i,j), MAT_ANY);
                }
            }
        }
    }
}

static void clear_env_map()
{
    env.map.init(map_cell());
}

static void clear_clouds()
{
    for (int clouty = 0; clouty < MAX_CLOUDS; ++clouty)
        delete_cloud( clouty );
    env.cgrid.init(EMPTY_CLOUD);
}

static bool grab_follower_at(const coord_def &pos)
{
    if (pos == you.pos())
        return (false);

    monsters *fmenv = monster_at(pos);
    if (!fmenv || !fmenv->alive())
        return (false);

    // monster has to be already tagged in order to follow:
    if (!testbits( fmenv->flags, MF_TAKING_STAIRS ))
        return (false);

#if DEBUG_DIAGNOSTICS
    mprf(MSGCH_DIAGNOSTICS, "%s is following to %s.",
         fmenv->name(DESC_CAP_THE, true).c_str(),
         level_id::current().describe().c_str());
#endif
    fmenv->set_transit(level_id::current());
    fmenv->destroy_inventory();
    monster_cleanup(fmenv);
    return (true);
}

static void grab_followers()
{
    const bool can_follow = level_type_allows_followers(you.level_type);
    for (int i = you.x_pos - 1; i < you.x_pos + 2; i++)
    {
        for (int j = you.y_pos - 1; j < you.y_pos + 2; j++)
        {
            if (i == you.x_pos && j == you.y_pos)
                continue;

            if (mgrd[i][j] == NON_MONSTER)
                continue;

            monsters *fmenv = &menv[mgrd[i][j]];

            if (fmenv->type == MONS_PLAYER_GHOST
                && fmenv->hit_points < fmenv->max_hit_points / 2)
            {
                mpr("The ghost fades into the shadows.");
                monster_teleport(fmenv, true);
                continue;
            }
        }
    }

    if (can_follow)
    {
        memset(travel_point_distance, 0, sizeof(travel_distance_grid_t));
        std::vector<coord_def> places[2];
        int place_set = 0;
        places[place_set].push_back(you.pos());
        while (!places[place_set].empty())
        {
            for (int i = 0, size = places[place_set].size(); i < size; ++i)
            {
                const coord_def &p = places[place_set][i];
                coord_def fp;
                for (fp.x = p.x - 1; fp.x <= p.x + 1; ++fp.x)
                    for (fp.y = p.y - 1; fp.y <= p.y + 1; ++fp.y)
                    {
                        if (!in_bounds(fp) || travel_point_distance[fp.x][fp.y])
                            continue;
                        travel_point_distance[fp.x][fp.y] = 1;
                        if (grab_follower_at(fp))
                            places[!place_set].push_back(fp);
                    }
            }
            places[place_set].clear();
            place_set = !place_set;
        }
    }

    // Clear flags on the followers that didn't make it.
    for (int i = 0; i < MAX_MONSTERS; ++i)
    {
        monsters *mons = &menv[i];
        if (!mons->alive())
            continue;
        mons->flags &= ~MF_TAKING_STAIRS;
    }
}

// Should be called after grab_followers(), so that items carried by
// followers won't be considered lost.
static void do_lost_items(level_area_type old_level_type)
{
    if (old_level_type == LEVEL_DUNGEON)
        return;

    for (int i = 0; i < MAX_ITEMS; i++)
    {
        item_def& item(mitm[i]);

        if (!is_valid_item(item))
            continue;

        // Item is in player intentory, so it's not lost.
        if (item.x == -1 && item.y == -1)
            continue;

        item_was_lost(item);
    }
}

bool load( dungeon_feature_type stair_taken, load_mode_type load_mode,
           level_area_type old_level_type, char old_level,
           branch_type old_branch )
{
    unwind_var<dungeon_feature_type> stair(
        you.transit_stair, stair_taken, DNGN_UNSEEN);
    unwind_bool ylev(you.entering_level, true, false);

    // Going up/down stairs, going through a portal, or being banished
    // means the previous x/y movement direction is no longer valid.
    you.reset_prev_move();

    const bool make_changes =
        (load_mode != LOAD_RESTART_GAME && load_mode != LOAD_VISITOR);

    bool just_created_level = false;

    std::string cha_fil = make_filename( you.your_name, you.your_level,
                                         you.where_are_you,
                                         you.level_type,
                                         false );

    if ((you.level_type == LEVEL_DUNGEON && old_level_type == LEVEL_DUNGEON)
        || load_mode == LOAD_START_GAME)
    {
        if (tmp_file_pairs[you.your_level][you.where_are_you] == false)
        {
            // make sure old file is gone
            unlink(cha_fil.c_str());

            // save the information for later deletion -- DML 6/11/99
            tmp_file_pairs[you.your_level][you.where_are_you] = true;
        }
    }

    you.prev_targ     = MHITNOT;
    you.prev_grd_targ = coord_def(0, 0);

    // We clear twice - on save and on load.
    // Once would be enough...
    if (make_changes)
        clear_clouds();

    // Lose all listeners.
    dungeon_events.clear();

    // This block is to grab followers and save the old level to disk.
    if (load_mode == LOAD_ENTER_LEVEL && old_level != -1)
    {
        grab_followers();

        if (old_level_type == LEVEL_DUNGEON)
            save_level( old_level, LEVEL_DUNGEON, old_branch );
    }

    if ( make_changes )
        do_lost_items(old_level_type);

    // Try to open level savefile.
    FILE *levelFile = fopen(cha_fil.c_str(), "rb");

    // GENERATE new level when the file can't be opened:
    if (levelFile == NULL)
    {
        ASSERT( load_mode != LOAD_VISITOR );
        env.turns_on_level = -1;
        builder( you.your_level, you.level_type );
        just_created_level = true;

        if ((you.your_level > 1 || you.level_type != LEVEL_DUNGEON)
            && one_chance_in(3))
        {
            load_ghost();
        }
        env.turns_on_level = 0;
        // sanctuary
        env.sanctuary_pos  = coord_def(-1, -1);
        env.sanctuary_time = 0;
    }
    else
    {
        // BEGIN -- must load the old level : pre-load tasks

        // LOAD various tags
        char majorVersion = 0;
        char minorVersion = 0;

        if (!determine_level_version( levelFile, majorVersion, minorVersion ))
        {
            end(-1, false, "\nLevel file appears to be invalid.\n");
        }

        restore_level_version( levelFile, majorVersion, minorVersion );

        // sanity check - EOF
        if (!feof( levelFile ))
        {
            end(-1, false, "\nIncomplete read of \"%s\" - aborting.\n",
                cha_fil.c_str());
        }

        fclose( levelFile );

        // POST-LOAD tasks :
        link_items();
        redraw_all();
    }

    if (load_mode == LOAD_START_GAME)
        just_created_level = true;

    // closes all the gates if you're on the way out
    if (you.char_direction == GDT_ASCENDING &&
        you.level_type != LEVEL_PANDEMONIUM)
        close_level_gates();

    if (just_created_level)
        clear_env_map();

    // Here's the second cloud clearing, on load (see above)
    if ( make_changes )
    {
        clear_clouds();
        if (you.level_type != LEVEL_ABYSS)
            place_player_on_stair(old_branch, stair_taken);
        else
            you.moveto(45, 35);
    }
    crawl_view.set_player_at(you.pos(), true);

    // This should fix the "monster occuring under the player" bug?
    if (make_changes && mgrd[you.x_pos][you.y_pos] != NON_MONSTER)
        monster_teleport(&menv[mgrd[you.x_pos][you.y_pos]], true, true);

    // actually "move" the followers if applicable
    if (level_type_allows_followers(you.level_type)
        && load_mode == LOAD_ENTER_LEVEL)
    {
        place_followers();
    }                       // end of moving followers

    // Load monsters in transit.
    if (load_mode == LOAD_ENTER_LEVEL)
    {
        place_transiting_monsters();
        place_transiting_items();
    }

    redraw_all();

    sanity_test_monster_inventory();

    if (load_mode != LOAD_VISITOR)
        dungeon_events.fire_event(DET_ENTERING_LEVEL);

    // Things to update for player entering level
    if (load_mode == LOAD_ENTER_LEVEL)
    {
        if (just_created_level)
            level_welcome_messages();

        // Activate markers that want activating, but only when
        // entering a new level in an existing game. If we're starting
        // a new game, or reloading an existing game,
        // markers are activated in acr.cc.
        env.markers.activate_all();

        // update corpses and fountains
        if (env.elapsed_time != 0.0 && !just_created_level)
            update_level( you.elapsed_time - env.elapsed_time );

        // Centaurs have difficulty with stairs
        int timeval = ((you.species != SP_CENTAUR) ? player_movement_speed()
                                                   : 15);

        // new levels have less wary monsters:
        if (just_created_level)
            timeval /= 2;

        timeval -= (stepdown_value( check_stealth(), 50, 50, 150, 150 ) / 10);

#if DEBUG_DIAGNOSTICS
        mprf(MSGCH_DIAGNOSTICS, "arrival time: %d", timeval );
#endif

        if (timeval > 0)
        {
            you.time_taken = timeval;
#ifndef USE_TILE
            viewwindow(true, false);
#endif
            handle_monsters();
        }
    }

    // Save the created/updated level out to disk:
    if ( make_changes )
        save_level( you.your_level, you.level_type, you.where_are_you );

    setup_environment_effects();

    // Inform user of level's annotation.
    if (load_mode != LOAD_VISITOR
        && !get_level_annotation().empty()
        && !crawl_state.level_annotation_shown)
    {
        mprf(MSGCH_PLAIN, YELLOW, "Level annotation: %s",
             get_level_annotation().c_str());
    }

    if (load_mode != LOAD_VISITOR)
        crawl_state.level_annotation_shown = false;

    if ( make_changes )
    {
        // Update PlaceInfo entries
        PlaceInfo& curr_PlaceInfo = you.get_place_info();
        PlaceInfo  delta;

        if (load_mode == LOAD_START_GAME ||
            (load_mode == LOAD_ENTER_LEVEL &&
             (old_branch != you.where_are_you ||
              old_level_type != you.level_type)))
            delta.num_visits++;

        if (just_created_level)
            delta.levels_seen++;

        you.global_info += delta;
        you.global_info.assert_validity();

        curr_PlaceInfo += delta;
        curr_PlaceInfo.assert_validity();
    }

    if (just_created_level)
        you.attribute[ATTR_ABYSS_ENTOURAGE] = 0;

    if ( load_mode != LOAD_VISITOR )
        dungeon_events.fire_event(DET_ENTERED_LEVEL);

    return just_created_level;
}                               // end load()

void save_level(int level_saved, level_area_type old_ltype,
                branch_type where_were_you)
{
    std::string cha_fil = make_filename( you.your_name, level_saved,
                                         where_were_you, old_ltype,
                                         false );

    you.prev_targ     = MHITNOT;
    you.prev_grd_targ = coord_def(0, 0);

    FILE *saveFile = fopen(cha_fil.c_str(), "wb");

    if (saveFile == NULL)
    {
        end(-1, true, "Unable to open \"%s\" for writing",
                 cha_fil.c_str());
    }

    // nail all items to the ground
    fix_item_coordinates();

    write_tagged_file( saveFile, SAVE_MAJOR_VERSION,
                       LEVEL_MINOR_VERSION, TAGTYPE_LEVEL );

    fclose(saveFile);

    DO_CHMOD_PRIVATE(cha_fil.c_str());
}                               // end save_level()


void save_game(bool leave_game, const char *farewellmsg)
{
    unwind_bool saving_game(crawl_state.saving_game, true);

    /* Stashes */
    std::string stashFile = get_savedir_filename( you.your_name, "", "st" );
    FILE *stashf = fopen(stashFile.c_str(), "wb");
    if (stashf)
    {
        writer outf(stashf);
        StashTrack.save(outf);
        fclose(stashf);
        DO_CHMOD_PRIVATE(stashFile.c_str());
    }

#ifdef CLUA_BINDINGS
    /* lua */
    std::string luaFile = get_savedir_filename( you.your_name, "", "lua" );
    clua.save(luaFile.c_str());
    // note that luaFile may not exist
    DO_CHMOD_PRIVATE(luaFile.c_str());
#endif

    /* kills */
    std::string killFile = get_savedir_filename( you.your_name, "", "kil" );
    FILE *killf = fopen(killFile.c_str(), "wb");
    if (killf)
    {
        writer outf(killf);
        you.kills->save(outf);
        fclose(killf);
        DO_CHMOD_PRIVATE(killFile.c_str());
    }

    /* travel cache */
    std::string travelCacheFile = get_savedir_filename(you.your_name,"","tc");
    FILE *travelf = fopen(travelCacheFile.c_str(), "wb");
    if (travelf)
    {
        writer outf(travelf);
        travel_cache.save(outf);
        fclose(travelf);
        DO_CHMOD_PRIVATE(travelCacheFile.c_str());
    }

    /* notes */
    std::string notesFile = get_savedir_filename(you.your_name, "", "nts");
    FILE *notesf = fopen(notesFile.c_str(), "wb");
    if (notesf)
    {
        writer outf(notesf);
        save_notes(outf);
        fclose(notesf);
        DO_CHMOD_PRIVATE(notesFile.c_str());
    }

    /* tutorial */

    std::string tutorFile = get_savedir_filename(you.your_name, "", "tut");
    FILE *tutorf = fopen(tutorFile.c_str(), "wb");
    if (tutorf)
    {
        writer outf(tutorf);
        save_tutorial(outf);
        fclose(tutorf);
        DO_CHMOD_PRIVATE(tutorFile.c_str());
    }

    std::string charFile = get_savedir_filename(you.your_name, "", "sav");
    FILE *charf = fopen(charFile.c_str(), "wb");
    if (!charf)
        end(-1, true, "Unable to open \"%s\" for writing!\n", charFile.c_str());

    write_tagged_file( charf, SAVE_MAJOR_VERSION,
                       YOU_MINOR_VERSION, TAGTYPE_PLAYER );

    fclose(charf);
    DO_CHMOD_PRIVATE(charFile.c_str());

    // if just save, early out
    if (!leave_game)
        return;

    // must be exiting -- save level & goodbye!
    if (!you.entering_level)
        save_level(you.your_level, you.level_type, you.where_are_you);

    clrscr();

#ifdef SAVE_PACKAGE_CMD
    std::string basename = get_savedir_filename(you.your_name, "", "");
    char cmd_buff[1024];

    snprintf( cmd_buff, sizeof(cmd_buff),
              SAVE_PACKAGE_CMD, basename.c_str(), basename.c_str() );

    if (system( cmd_buff ) != 0)
    {
        cprintf( EOL "Warning: Zip command (SAVE_PACKAGE_CMD) returned"
                     "non-zero value!" EOL );
    }
    DO_CHMOD_PRIVATE ( (basename + PACKAGE_SUFFIX).c_str() );
#endif

#ifdef DGL_WHEREIS
    whereis_record("saved");
#endif
    end(0, false, farewellmsg? "%s" : "See you soon, %s!",
        farewellmsg? farewellmsg : you.your_name);
}                               // end save_game()

// Saves the game without exiting.
void save_game_state()
{
    save_game(false);
    if (crawl_state.seen_hups)
        save_game(true);
}

void load_ghost(void)
{
    char majorVersion;
    char minorVersion;

    std::string cha_fil = make_filename("bones", you.your_level,
                                        you.where_are_you,
                                        you.level_type,
                                        true );

    FILE *gfile = fopen(cha_fil.c_str(), "rb");

    if (gfile == NULL)
        return;                 // no such ghost.

    if (!determine_ghost_version(gfile, majorVersion, minorVersion))
    {
        fclose(gfile);
#if DEBUG_DIAGNOSTICS
        mprf(MSGCH_DIAGNOSTICS,
             "Ghost file \"%s\" seems to be invalid.", cha_fil.c_str());
        more();
#endif
        return;
    }

    if (majorVersion != SAVE_MAJOR_VERSION
        || minorVersion != GHOST_MINOR_VERSION)
    {

        fclose(gfile);
        unlink(cha_fil.c_str());
        return;
    }

    ghosts.clear();
    restore_ghost_version(gfile, majorVersion, minorVersion);

    // sanity check - EOF
    if (!feof(gfile))
    {
        fclose(gfile);
#if DEBUG_DIAGNOSTICS
        mprf(MSGCH_DIAGNOSTICS, "Incomplete read of \"%s\".",
                  cha_fil.c_str() );
        more();
#endif
        return;
    }

    fclose(gfile);

#if DEBUG_DIAGNOSTICS
        mpr( "Loaded ghost.", MSGCH_DIAGNOSTICS );
#endif

    // remove bones file - ghosts are hardly permanent.
    unlink(cha_fil.c_str());

    // translate ghost to monster and place.
    for (int imn = 0; imn < MAX_MONSTERS - 10 && !ghosts.empty(); imn++)
    {
        if (menv[imn].type != -1)
            continue;

        menv[imn].set_ghost(ghosts[0]);
        menv[imn].ghost_init();

        ghosts.erase(ghosts.begin());
    }
}

void restore_game(void)
{
    std::string charFile = get_savedir_filename(you.your_name, "", "sav");
    FILE *charf = fopen(charFile.c_str(), "rb");
    if (!charf )
        end(-1, true, "Unable to open %s for reading!\n", charFile.c_str() );

    char majorVersion = 0;
    char minorVersion = 0;

    if (!determine_version(charf, majorVersion, minorVersion))
        end(-1, false, "\nSavefile appears to be invalid.\n");

    restore_version(charf, majorVersion, minorVersion);

    // sanity check - EOF
    if (!feof(charf))
        end(-1, false, "\nIncomplete read of \"%s\" - aborting.\n",
            charFile.c_str());

    fclose(charf);

    std::string stashFile = get_savedir_filename( you.your_name, "", "st" );
    FILE *stashf = fopen(stashFile.c_str(), "rb");
    if (stashf)
    {
        reader inf(stashf);
        StashTrack.load(inf);
        fclose(stashf);
    }

#ifdef CLUA_BINDINGS
    std::string luaFile = get_savedir_filename( you.your_name, "", "lua" );
    clua.execfile( luaFile.c_str() );
#endif

    std::string killFile = get_savedir_filename( you.your_name, "", "kil" );
    FILE *killf = fopen(killFile.c_str(), "rb");
    if (killf)
    {
        reader inf(killf);
        you.kills->load(inf);
        fclose(killf);
    }

    std::string travelCacheFile = get_savedir_filename(you.your_name,"","tc");
    FILE *travelf = fopen(travelCacheFile.c_str(), "rb");
    if (travelf)
    {
        reader inf(travelf);
        travel_cache.load(inf);
        fclose(travelf);
    }

    std::string notesFile = get_savedir_filename(you.your_name, "", "nts");
    FILE *notesf = fopen(notesFile.c_str(), "rb");
    if (notesf)
    {
        reader inf(notesf);
        load_notes(inf);
        fclose(notesf);
    }

    /* tutorial */

    std::string tutorFile = get_savedir_filename(you.your_name, "", "tut");
    FILE *tutorf = fopen(tutorFile.c_str(), "rb");
    if (tutorf)
    {
        reader inf(tutorf);
        load_tutorial(inf);
        fclose(tutorf);
    }
}

static void _restore_level(const level_id &original)
{
    // Reload the original level.
    you.where_are_you = original.branch;
    you.your_level = original.absdepth();
    you.level_type = original.level_type;

    load( DNGN_STONE_STAIRS_DOWN_I, LOAD_VISITOR,
          you.level_type, you.your_level, you.where_are_you );
}

// Given a level in the dungeon (i.e. level_type == LEVEL_DUNGEON),
// returns true if the level has been created already in this game.
// Asserts if the level_type is not LEVEL_DUNGEON.
bool is_existing_level(const level_id &level)
{
    ASSERT(level.level_type == LEVEL_DUNGEON);
    return (tmp_file_pairs[level.absdepth()][level.branch]);
}

// Applies an operation (applicator) after switching to the specified level.
// If preserve_current is true, will reload the original level after
// modifying the target level.
//
// If the target level has not already been visited by the player, this
// function will assert.
//
bool apply_to_level(const level_id &level, bool preserve_current,
                    bool (*applicator)())
{
    ASSERT(is_existing_level(level));

    const level_id original = level_id::current();
    if (level != original)
    {
        if (preserve_current)
            save_level(you.your_level, you.level_type, you.where_are_you);

        you.where_are_you = level.branch;
        you.your_level = level.absdepth();
        you.level_type = level.level_type;

        // Load the dungeon level...
        load( DNGN_STONE_STAIRS_DOWN_I, LOAD_VISITOR,
              LEVEL_DUNGEON, original.absdepth(),
              original.branch );
    }

    // Apply the change.
    const bool result = applicator();

    if (level != original)
    {
        // And save it back.
        save_level(you.your_level, you.level_type, you.where_are_you);

        if (preserve_current)
            _restore_level(original);
    }

    return (result);
}

bool apply_to_all_dungeons(bool (*applicator)())
{
    const level_id original = level_id::current();

    // Apply to current level, then save it out.
    bool success = applicator();
    save_level(original.absdepth(), original.level_type, original.branch);

    for ( int i = 0; i < MAX_LEVELS; ++i )
    {
        for ( int j = 0; j < NUM_BRANCHES; ++j )
        {
            const branch_type br = static_cast<branch_type>(j);
            const level_id thislevel(br, subdungeon_depth(br, i));

            if (!is_existing_level(thislevel))
                continue;

            // Don't apply to the original level - already done up top.
            if (original == thislevel)
                continue;

            if (apply_to_level(thislevel, false, applicator))
                success = true;
        }
    }

    _restore_level(original);

    return success;
}

static bool determine_version( FILE *restoreFile,
                               char &majorVersion, char &minorVersion )
{
    // read first two bytes.
    char buf[2];
    if (read2(restoreFile, buf, 2) != 2)
        return false;               // empty file?

    // otherwise, read version and validate.
    majorVersion = buf[0];
    minorVersion = buf[1];

    if (majorVersion == SAVE_MAJOR_VERSION)
        return true;

    return false;   // if it's not 0, no idea
}

static void restore_version( FILE *restoreFile,
                             char majorVersion, char minorVersion )
{
    // assuming the following check can be removed once we can read all
    // savefile versions.
    if (majorVersion != SAVE_MAJOR_VERSION)
    {
        end(-1, false, "\nSorry, this release cannot read a v%d.%d savefile.\n",
            majorVersion, minorVersion);
    }

    switch(majorVersion)
    {
        case SAVE_MAJOR_VERSION:
            restore_tagged_file(restoreFile, TAGTYPE_PLAYER, minorVersion);
            break;
        default:
            break;
    }
}

// generic v4 restore function
static void restore_tagged_file( FILE *restoreFile, int fileType,
                                 char minorVersion )
{
    char tags[NUM_TAGS];
    tag_set_expected(tags, fileType);

    while (true)
    {
        tag_type tt = tag_read(restoreFile, minorVersion);
        if (tt == TAG_NO_TAG)
            break;

        tags[tt] = 0;                // tag read
        if (fileType == TAGTYPE_PLAYER_NAME)
            break;
    }

    // go through and init missing tags
    for (int i = 0; i < NUM_TAGS; i++)
        if (tags[i] == 1)           // expected but never read
            tag_missing(i, minorVersion);
}

static bool determine_level_version( FILE *levelFile,
                                     char &majorVersion, char &minorVersion )
{
    // read first two bytes.
    char buf[2];
    if (read2(levelFile, buf, 2) != 2)
        return false;               // empty file?

    // otherwise, read version and validate.
    majorVersion = buf[0];
    minorVersion = buf[1];

    if (majorVersion == SAVE_MAJOR_VERSION)
        return true;

    return false;   // if its not SAVE_MAJOR_VERSION, no idea
}

static void restore_level_version( FILE *levelFile,
                                   char majorVersion, char minorVersion )
{
    // assuming the following check can be removed once we can read all
    // savefile versions.
    if (majorVersion != SAVE_MAJOR_VERSION)
    {
        end(-1, false,
            "\nSorry, this release cannot read a v%d.%d level file.\n",
            majorVersion, minorVersion);
    }

    switch(majorVersion)
    {
        case SAVE_MAJOR_VERSION:
            restore_tagged_file(levelFile, TAGTYPE_LEVEL, minorVersion);
            break;
        default:
            break;
    }
}

static bool determine_ghost_version( FILE *ghostFile,
                                     char &majorVersion, char &minorVersion )
{
    // read first two bytes.
    char buf[2];
    if (read2(ghostFile, buf, 2) != 2)
        return false;               // empty file?

    // otherwise, read version and validate.
    majorVersion = buf[0];
    minorVersion = buf[1];

    reader inf(ghostFile);
    // check for the DCSS ghost signature.
    if (unmarshallShort(inf) != GHOST_SIGNATURE)
        return (false);

    if (majorVersion == SAVE_MAJOR_VERSION
        && minorVersion <= GHOST_MINOR_VERSION)
    {
        // Discard three more 32-bit words of padding.
        inf.read(NULL, 3*4);
        return !feof(ghostFile);
    }

    return false;   // if its not SAVE_MAJOR_VERSION, no idea!
}

static void restore_ghost_version( FILE *ghostFile,
                                   char majorVersion, char minorVersion )
{
    switch(majorVersion)
    {
    case SAVE_MAJOR_VERSION:
        restore_tagged_file(ghostFile, TAGTYPE_GHOST, minorVersion);
        break;
    default:
        break;
    }
}

void save_ghost( bool force )
{
    if (!force && (you.your_level < 2 || you.is_undead))
        return;

    std::string cha_fil = make_filename( "bones", you.your_level,
                                         you.where_are_you,
                                         you.level_type,
                                         true );

    FILE *gfile = fopen(cha_fil.c_str(), "rb");

    // don't overwrite existing bones!
    if (gfile != NULL)
    {
        fclose(gfile);
        return;
    }

    ghosts = ghost_demon::find_ghosts();

    gfile = lk_open("wb", cha_fil);

    if (gfile == NULL)
    {
        mprf("Error creating ghost file: %s", cha_fil.c_str());
        more();
        return;
    }

    write_tagged_file( gfile, SAVE_MAJOR_VERSION,
                       GHOST_MINOR_VERSION, TAGTYPE_GHOST,
                       true );

    lk_close(gfile, "wb", cha_fil);

#if DEBUG_DIAGNOSTICS
    mpr( "Saved ghost.", MSGCH_DIAGNOSTICS );
#endif

    DO_CHMOD_PRIVATE(cha_fil.c_str());
}                               // end save_ghost()


void generate_random_demon()
{
    int rdem = 0;
    for (rdem = 0; rdem < MAX_MONSTERS + 1; rdem++)
    {
        if (rdem == MAX_MONSTERS)
            return;

        if (menv[rdem].type == MONS_PANDEMONIUM_DEMON)
            break;
    }

    ghost_demon pandemon;
    pandemon.init_random_demon();
    menv[rdem].set_ghost(pandemon);
    menv[rdem].pandemon_init();
}                               // end generate_random_demon()

////////////////////////////////////////////////////////////////////////////
// Locking for multiuser systems

// first, some file locking stuff for multiuser crawl
#ifdef USE_FILE_LOCKING

bool lock_file_handle( FILE *handle, int type )
{
    struct flock  lock;
    int           status;

    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0;
    lock.l_type = type;

#ifdef USE_BLOCKING_LOCK
    status = fcntl( fileno( handle ), F_SETLKW, &lock );
#else
    for (int i = 0; i < 30; i++)
    {
        status = fcntl( fileno( handle ), F_SETLK, &lock );

        // success
        if (status == 0)
            break;

        // known failure
        if (status == -1 && (errno != EACCES && errno != EAGAIN))
            break;

        perror( "Problems locking file... retrying..." );
        delay( 1000 );
    }
#endif

    return (status == 0);
}

bool unlock_file_handle( FILE *handle )
{
    struct flock  lock;
    int           status;

    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0;
    lock.l_type = F_UNLCK;

#ifdef USE_BLOCKING_LOCK

    status = fcntl( fileno( handle ), F_SETLKW, &lock );

#else

    for (int i = 0; i < 30; i++)
    {
        status = fcntl( fileno( handle ), F_SETLK, &lock );

        // success
        if (status == 0)
            break;

        // known failure
        if (status == -1 && (errno != EACCES && errno != EAGAIN))
            break;

        perror( "Problems unlocking file... retrying..." );
        delay( 1000 );
    }

#endif

    return (status == 0);
}

#endif

FILE *lk_open(const char *mode, const std::string &file)
{
    FILE *handle = fopen(file.c_str(), mode);
#ifdef SHARED_FILES_CHMOD_PUBLIC
    chmod(file.c_str(), SHARED_FILES_CHMOD_PUBLIC);
#endif

#ifdef USE_FILE_LOCKING
    int locktype = F_RDLCK;
    if (mode && mode[0] != 'r')
        locktype = F_WRLCK;

    if (handle && !lock_file_handle( handle, locktype ))
    {
        perror( "Could not lock file... " );
        fclose( handle );
        handle = NULL;
    }
#endif
    return handle;
}

void lk_close(FILE *handle, const char *mode, const std::string &file)
{
    UNUSED( mode );

    if (handle == NULL || handle == stdin)
        return;

#ifdef USE_FILE_LOCKING
    unlock_file_handle( handle );
#endif

    // actually close
    fclose(handle);

#ifdef SHARED_FILES_CHMOD_PUBLIC
    if (mode && mode[0] == 'w')
        chmod(file.c_str(), SHARED_FILES_CHMOD_PUBLIC);
#endif
}

/////////////////////////////////////////////////////////////////////////////
// file_lock
//
// Locks a named file (usually an empty lock file), creating it if necessary.

file_lock::file_lock(const std::string &s, const char *_mode, bool die_on_fail)
    : handle(NULL), mode(_mode), filename(s)
{
#ifdef USE_FILE_LOCKING
    if (!(handle = lk_open(mode, filename)) && die_on_fail)
        end(1, true, "Unable to open lock file \"%s\"", filename.c_str());
#endif
}

file_lock::~file_lock()
{
#ifdef USE_FILE_LOCKING
    if (handle)
        lk_close(handle, mode, filename);
#endif
}
