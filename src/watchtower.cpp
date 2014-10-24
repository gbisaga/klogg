#include "watchtower.h"

#include <algorithm>

#include "log.h"

namespace {
    bool isSymLink( const std::string& file_name );
    std::string directory_path( const std::string& path );
};

WatchTower::WatchTower( std::shared_ptr<WatchTowerDriver> driver )
    : thread_(), driver_( driver ),
    heartBeat_(std::shared_ptr<void>((void*) 0xDEADC0DE, [] (void*) {}))
{
    running_ = true;
    thread_ = std::thread( &WatchTower::run, this );
}

WatchTower::~WatchTower()
{
    running_ = false;
    thread_.join();
}

WatchTower::Registration WatchTower::addFile(
        const std::string& file_name,
        std::function<void()> notification )
{
    LOG(logDEBUG) << "WatchTower::addFile " << file_name;

    std::lock_guard<std::mutex> lock( observers_mutex_ );

    ObservedFile* existing_observed_file =
        observed_file_list_.searchByName( file_name );

    std::shared_ptr<std::function<void()>> ptr( new std::function<void()>(std::move( notification ) ) );

    if ( ! existing_observed_file )
    {
        WatchTowerDriver::SymlinkId* symlink_id = nullptr;

        auto file_id = driver_->addFile( file_name );

        if ( isSymLink( file_name ) )
        {
            // We want to follow the name (as opposed to the inode)
            // so we watch the symlink as well.
            symlink_id = driver_->addSymlink( file_name );
        }

        auto new_file = observed_file_list_.addNewObservedFile(
                ObservedFile( file_name, ptr, wd, symlink_wd ) );

        auto dir = observed_file_list_.watchedDirectoryForFile( file_name );
        if ( ! dir )
        {
            LOG(logDEBUG) << "INotifyWatchTower::addFile dir for " << file_name
                << " not watched, adding...";
            dir = observed_file_list_.addWatchedDirectoryForFile( file_name );

            dir->dir_wd_ = driver_->addDir( dir->path );
        }

        new_file->dir_ = dir;
    }
    else
    {
        existing_observed_file->addCallback( ptr );
    }

    std::weak_ptr<void> weakHeartBeat(heartBeat_);

    // Returns a shared pointer that removes its own entry
    // from the list of watched stuff when it goes out of scope!
    // Uses a custom deleter to do the work.
    return std::shared_ptr<void>( 0x0, [this, ptr, weakHeartBeat] (void*) {
            if ( auto heart_beat = weakHeartBeat.lock() )
                removeNotification( this, ptr );
            } );
}

//
// Private functions
//

// Called by the dtor for a registration object
void WatchTower::removeNotification(
        WatchTower* watch_tower, std::shared_ptr<void> notification )
{
    LOG(logDEBUG) << "WatchTower::removeNotification";

    std::lock_guard<std::mutex> lock( watch_tower->observers_mutex_ );

    auto file =
        watch_tower->observed_file_list_.removeCallback( notification );

    if ( file )
    {
        driver_->removeFile( file->file_wd_ );
        driver_->removeSymlink( file->symlink_wd_ );
    }
}

// Run in its own thread
void INotifyWatchTower::run()
{
    struct pollfd fds[1];

    fds[0].fd     = inotify_fd;
    fds[0].events = POLLIN;

    while ( running_ )
    {
        std::vector<ObservedFile*> files = waitAndProcessEvents( observed_file_list_, observers_mutex_ );

    }
}

namespace {
    bool isSymLink( const std::string& file_name )
    {
        struct stat buf;

        lstat( file_name.c_str(), &buf );
        return ( S_ISLNK(buf.st_mode) );
    }
};