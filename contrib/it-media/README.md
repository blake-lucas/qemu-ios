# Native media import (3.1 to 7.1.2)

`itmedia` adds one staged song or movie through the guest's own MusicLibrary framework.
It preserves existing songs and lets iOS write its SQLite tables, indexes,
locations, Purchased playlist and backup files. It does not generate a legacy
iTunesDB or rewrite the library on the host.

This changes the original plan's D.1/D.2 implementation choice: iOS 3.x uses
`iTunes_Control/iTunes/iTunes Library.itlp/*.itdb`, and its MusicLibrary answers
`-[MLMusicLibrary_SQL insertItemFromPurchaseFolder:withItemProperties:]`. The helper checks for that
service at runtime, not for a build. Verified with tags, cover and playback on the iPod's 3.1.2 and 3.1.3
and the iPad's 3.2 (Light Touch `tests/sessions/check-media-native.py --single`, since retired; `sessions single
--media` now imports a song, a movie and a photo and reads the libraries back, also after a reboot).

4.0 to 4.2.1 keep the insert and the itlp files and sign each database file (`Locations.itdb.cbk` and the rest).
Two things decide whether an insert stays:

- 4.x MusicLibrary opens the library for writing only in Music (`com.apple.mobileipod`), iPod Out and
  `itdbprepserver` (`MLProcessHasDatabaseWritePermissions`: the main bundle's identifier, else the process name).
  Anywhere else its inserts and sort-map updates hit "attempt to write a readonly database", the half-written item
  fails its signature and post-processing deletes it ("the music library dropped the import"). Purchases reach
  the library through Music on the device, so `itmedia` is linked with an `__info_plist` naming it Music
  (`itmedia-Info.plist`); 3.x and 5.x on have no such check.
- A library the helper's own process has just created fails its own size check at the next open ("ERROR-cbk:
  filesize mismatch") and refuses the insert; a second process imports into it. On a device with no library the
  helper commits the new one and executes itself again.

Earlier notes blamed FairPlay in the emulator (`cfdcb statusError = -42180` in itdbprepserver, which has no
`fairplay-client` entitlement on 4.2.1): with the two points above the library keeps the item through
post-processing, Music's own load and a reboot (8C148 on the iPhone 4).

5.x (verified on the iPad's 5.1.1, 9B206, with tags, cover, the Songs list and playback) replaced the library
with ML3 (`MediaLibrary.sqlitedb`) and has no purchase-folder insert. The helper uses ML3's own importer,
`-[ML3TrackImporter importTrack:withCompletionBlock:]` with an `MLTrackImport` (`setValue:forEntityProperty:` with
the exported `ML3TrackProperty*` names, `setAssetFilePath:`, `setArtworkData:`, `setMediaType:`), on a writer from
`-[ML3MusicLibrary initWithPath:enableWrites:]` (`sharedLibrary` is read-only to other processes; 7.x has
`initWithPath:readOnly:populateUnitTestTablesBlock:` instead, and the same importer). Two things decide
whether Music shows and plays the row:

- the media type is ML3's, not MPMediaType's: 9B206 MusicLibrary's `MLMediaType` description names bit 8 "Song" and
  Music's song queries select `item.media_type & 8` (verbose ML3 query log). A row with 1 makes Music show its tabs
  but list nothing. A movie is 0x800, what `MLMediaTypeForITMediaKind` makes of iTunes' kind 2;
- `item_extra.integrity` (`ML3TrackIntegrityVerify`) must be set, or Music lists the song but won't play it. The
  importer computes it with FairPlay (`FairPlayCandyBarCompute`), which answers only processes with a
  `fairplay-client` entitlement (it logs -42180 otherwise). `itmedia.entitlements` carries Music's client ID,
  1974055701, the process the helper names itself as (4.3's FairPlay refuses atc's, 177554772, which 5.x to 7.1.2
  also take); the helper fails if the column stays NULL.

The completion block is a global block literal whose isa is `_NSConcreteGlobalBlock` itself: 7.x's importer retains
it, and the class's first word in its place crashed there (5.x and 6.x never looked).

5.x covers are rendered by the importer under `iTunes_Control/iTunes/Artwork/NN/` (`artwork_info` lists them).

4.3 to 4.3.5 have no purchase-folder insert: their MusicLibrary imports through ML3's importer (itunesstored's
`AddItemToIPodLibraryOperation` does, on `+[ML3MusicLibrary sharedLibrary]`), but over the itlp library
(`+[ML3MusicLibrary mainLibraryPath]` is `iTunes Library.itlp/Library.itdb`; `+[MusicLibrary sharedMusicLibraryClass]`
is `ML3MusicLibraryImpl`). The library 4.3 itself creates (`_MLSDBLoadAndInitSharedInstances`: "no library exists",
then the older MLS tables, user version 8 in `db_schema_upgrade_info`) lacks what the importer writes
(`item.album_artist_pid`, `sort_map`, the newer `album`, `location` and `video_info` columns). On a phone the library
gets them from iTunes: 4.3 has no migration for it ("Syncing is required to upgrade your database schema",
`+[MLMusicLibrary_SQL isDatabaseSchemaUnsupported:]`), and `-[ML3MusicLibrary executeITunesCommands]` runs the SQL an
iTunes sync leaves. So before its first import the helper gives the library the tables and columns 4.3's ML3 defines:
`+[ML3MusicLibrary buildDatabaseTablesUsingHandle:]` creates ML3's schema on an empty in-memory database, and each
table or column the device's library lacks is added through `sharedLibrary`'s own connection (`executeSQL:`), with
the locations in `Locations.itdb` as the library keeps them. Rows are never rewritten. It then imports as on 5.x
and runs itdbprepserver's post-processing (`executeITunesCommands`, `executeITDBPostProcessing`, `updateSortMap`), as
after a purchase. Photos save through AssetsLibrary as on 4.0 to 4.2.1.

On 3.1 to 3.1.3 on the iPhone 3GS and the iPod touch 3G, Music (MobileMusicPlayer, which serves MediaPlayer's
library queries to other processes through `com.apple.mobileipod.MPMediaLibraryMigServer`) runs in the background
from boot, before anything is opened, and holds a read transaction on `Library.itdb` for as long as it runs (the
`F_GETLK` owner of the shared range of SQLite's lock bytes, at every point from the guest agent's first answer to the
import). The insert's commit then waits without end in SQLite's busy handler. The iPod touch 2G's 3.1.3 has no
Music process at boot; of SpringBoard's capabilities it lacks the other two's `voice-control`, whose iPod plug-in
(VoiceServices' iPod.vsplugin) reads the library, the likely client, not traced. An iTunes sync starts with `com.apple.itunes-mobdev.syncWillStart`, and Music
quits on it, as it does for a sync; nothing else needed it. So when another process holds that range, the helper
posts that notification, waits for the range to clear (up to 30 s), inserts, and posts
`com.apple.itunes-mobdev.syncDidFinish` after the import or a refusal; Music starts again in the background after
it. SpringBoard shows no sync screen for the notification alone (Home stays frontmost). The cost: a song Music
was playing would stop, as for a sync.

Not offered by Light Touch (its MediaSupport), and why:

- 2.x and 3.0 have neither (and 3.0 needs a legacy-linked build).

Both helpers are signed with `ldid -S`: 3.2+ AMFI runs nothing unsigned. Upstream
[libgpod's SQLite notes](https://github.com/fadingred/libgpod/blob/master/README.sqlite)
describe the format transition. No libgpod implementation is included here.

Build:

```sh
ARMV6_SDK=/path/to/iPhoneOS3.1.3.sdk bash contrib/it-media/build.sh
```

Stage a readable MP3, M4A or WAV for a song, or M4V, MP4 or MOV for a movie, under
`/var/mobile/Media/LightTouch/<staging-id>/<filename>`. The two directories must
be ordinary directories accessible to mobile (uid/gid 501); file mode 0644 is
sufficient. IDs and filenames accept ASCII letters, digits, hyphens,
underscores and dots, up to 128 bytes, with no leading dot. Symlinks are refused.
The metadata plist is limited to 64 KiB:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict>
  <key>filename</key><string>audio.m4a</string>
  <key>title</key><string>Example song</string>
  <key>artist</key><string>Example artist</string>
  <key>album</key><string>Example album</string>
  <key>duration_ms</key><integer>6000</integer>
</dict></plist>
```

`cc -DITMEDIA_HOST_CHECK itmedia.c` builds the metadata mapping alone for the Mac:
`itmedia-host metadata.plist staging-id out.plist` writes the properties, artwork filename
and year the guest would use (Light Touch's `tests/offline/check-media-metadata.py`).

Run as root (drops to mobile) or mobile:

```sh
/tmp/itmedia /tmp/song.plist staging-id
```

Title, filename and duration are required. The helper rounds fractional milliseconds
to a positive whole-millisecond NSNumber at the native purchase boundary: 7E18
silently stores zero duration for a fractional value. Optional: `artist`, `album`,
`album_artist`, `composer`, `genre` (strings); `track_number`, `track_count`,
`disc_number`, `disc_count` (1..65535); `year` (1..9999); `compilation` (boolean);
`artwork`, exactly `artwork.jpg`, a JPEG cover staged beside the media (2 MiB maximum). They become
MusicLibrary's purchase-folder properties (`itemName`, `artistName`, `playlistName`,
`playlistArtistName`, `composerName`, `genre`, `trackNumber`, `trackCount`,
`discNumber`, `discCount`, `compilation`), as the 7E18
`insertItemFromPurchaseFolder:withItemProperties:` reads them. That insert has no
year property, so the helper sets `item.year` with one UPDATE by pid through
MusicLibrary's own writer connection (`MLSDBGetSharedRecordStore`; the library
uses its own SQLite VFS, so a second plain connection is read-only). No 7E18
trigger or index covers the column.
The optional `kind` is `song` by default; use `feature-movie` for a movie.
MusicLibrary sets the native media kind and video fields. The caller supplies
metadata and is responsible for validating the codecs; accepting a filename
extension does not mean the device can decode its contents.

Artwork: MusicLibrary reads a purchased item's cover from an ArtworkCache
(`artwork.db`, `artwork.pix`) under the decimal `artworkDBRecordID`, which the insert takes from
`itemId`. On 3.x the cache is `Purchases/MobileArtworkDB`; on 4.0 to 4.2.1 it is the `MobileArtworkDB`
beside the purchase's own media (8C148 `-[MLTrack mobileArtworkCacheFolder]`: the track's path, its last
component replaced), here `LightTouch/<staging-id>/MobileArtworkDB`; one in `Purchases` there is never read,
so Music, Now Playing and Cover Flow showed the placeholder. The helper picks the cache by whether
`MLTrack` has that method. A 4.x song imported before keeps no cover until it is imported again
(same staging ID: `already-imported`, the cover rendered where 4.x reads it). The helper sets
an unused `itemId` from the native library under the import lock and, before
inserting, renders the cover with `ArtworkCache_ProcessArtworkData`:
the device's own AlbumArt formats from lockdown's `com.apple.mobile.iTunes`
domain (3005 is 320x320). A cover that cannot be stored fails the import, which
can then be retried with the same staging ID.
Encrypted tracks, transcoding, playlists other than the native
Purchased list and a Mac movie-import interface are not implemented here.
The separate `itphoto` helper handles photos.

Success ends with `imported` or `already-imported`. The framework may also emit
diagnostics during first-time library creation. A nonzero exit preserves staged
media: a failed or interrupted request can have committed its library change.
Reconcile using the same staging ID and filename, never by blindly choosing a
new ID. Once imported, the staged path is the permanent media location and
must not be removed or overwritten. Imports through this helper serialize on
an advisory lock. Concurrent iTunes synchronization is not supported.

The retry query opens SQLite read-only and matches the file location to an
existing media item, including a movie. It avoids Music's private collation indexes with `NOT INDEXED`,
so a library that has been opened by Music remains queryable after a reboot.
It fails closed on database errors. This is duplicate reconciliation for the
same staged file, not content-based deduplication across different locations.

Native acceptance was `tests/ipod/test_media_guest.py` (retired; see git history
at 5508b504b8). The test imported generated AAC/MP3 and movie fixtures into a disposable overlay, rejected
malformed requests, reconciled duplicates, checked Music's SQLite records and
the public MediaPlayer song query in Harness, captured the Songs and playback
screens, verified stereo tones in host audio, and checked persistence and retry
reconciliation after a cold boot. It checked the movie's database classification
and duplicate reconciliation; movie playback is not covered. Compressed decoding is enabled to match Light
Touch. On 7E18 the third-party MediaPlayer query connects to Music's MIG service;
the test started Music before requesting the Harness count.

## Saved Photos helper

The same build script produces `itphoto`. Stage an upright, 8-bit baseline JPEG
as `/var/mobile/Media/LightTouch/<staging-id>/image.jpg`, then run
`/tmp/itphoto <staging-id>` as root or mobile. Input is limited to 16 MiB and
2048 pixels on each side, checked before UIKit decodes it. Light Touch prepares
this representation on the host. The helper uses 7E18 PhotoLibrary's
`PLCameraAlbum` save operation with `notifyingTargetWithPath:` (3.2's is
`addCapturedImage:withPreview:zoomFactor:…`, used with zoom 1 where 3.1's is absent) and waits up to
40 seconds for its callback. The local 3.1.3 SDK's Objective-C metadata confirms
the selector and argument layout; the helper checks its availability at runtime.
iOS creates the DCIM original, poster image and BTH/THM thumbnails.

From 4.0, where PLCameraAlbum has neither save, the helper uses AssetsLibrary's public save:
`-[ALAssetsLibrary writeImageDataToSavedPhotosAlbum:metadata:completionBlock:]` with the staged JPEG's own bytes
(4.0 has only `writeImageToSavedPhotosAlbum:orientation:completionBlock:`, given the decoded image, orientation
up), running the main run loop until the completion block names the new asset. Its identity is the asset's
`assets-library://asset/asset.JPG?id=…&ext=JPG` URL instead of a DCIM path, and a repeated request asks
`assetForURL:resultBlock:failureBlock:` whether it is still there (a nil asset: deleted, so it may import again).
From 6.0 Photos access goes through TCC; `itphoto.entitlements` carries `com.apple.private.tcc.allow`
`kTCCServicePhotos`, as atc's does, so no prompt is shown.

Before saving, the helper durably writes a `pending` receipt. A successful
callback records `done` and the saved DCIM original path, then removes the redundant staged JPEG; Photos
owns the registered copy. A repeated completed request returns `already-imported`
while that original exists. If the original or its album was deleted, the same
staging ID may import again. DCIM must still be an ordinary directory; malformed
paths, unexpected file types and inspection errors preserve the receipt and
refuse replay. The album and original are checked without following symlinks.
Legacy `done` receipts without a path keep their previous duplicate-only behavior.
A pending or malformed receipt refuses replay, because a killed process may
have saved the photo without recording completion. It preserves the staged
image for inspection. Do not delete receipts to force a retry without checking
Saved Photos first. Different staging IDs are independent imports.

`tests/ipod/test_photo_guest.py` (retired) checked registration, thumbnail files, original
dimensions/colors, completed and uncertain receipts, malformed input, and cold
persistence with two guest-confirmed shutdowns. It captured album/grid/full-size
screens and checked grid colors. The new saved-path deletion/reimport behavior
has offline coverage; this cleanup did not run the native guest suite.

`tests/slice/ipod-media-helpers.c` runs offline fixture checks for
completed, legacy, uncertain and malformed receipts, deleted originals/albums,
invalid storage entries, and song/movie identity queries. It compiles the
production receipt preflight and stops before calling native Photos, so it
does not validate the framework callback or image decoding.

Before confirming an import, the helper synchronously calls 7E18 ITSync's
`ITDBPrepServerPostProcessRun(NULL, 1)`. The second argument selects
`sendMessageAndReceiveReplyName:userInfo:` rather than the asynchronous send.
This finishes native sorting/index work before the user opens Music.

Without this step, Music's `SyncHelper` starts post-processing during launch
and can exit as its sync phase ends. An isolated interposition trace identifies
`-[SyncHelper _delayedTerminate]` at `0x57bc4`, calling UIKit's
`terminateWithSuccess` with status 0. It is not a volume-button crash. The native
Light Touch media test now requires Music to stay foreground through its first
Songs tab and volume setup; it no longer relaunches Music to recover.

Light Touch identifies newly prepared photos by the SHA-256-derived UUID of the
baseline JPEG, using the same identity helper as music. Re-preparing unchanged
content therefore reaches the same receipt rather than creating a duplicate.
Completed retries remove a restaged JPEG. Existing random-ID imports are not
retroactively indexed. New receipts reconcile deletion by the native DCIM path;
this is not a general bidirectional photo-library index. A path reused for a
different asset cannot be distinguished by these receipts.

Read-only reconciliation retries SQLITE_SCHEMA by reopening and repreparing (at
most three attempts). All other errors remain fatal. Artwork IDs are allocated
from the native library under the import lock, rather than truncating a staging
hash; repeat imports read the committed item's ID. Host covers are bounded
JPEGs at artwork.jpg, at most 2 MiB; the cache directories must be real directories.
Old imports with artwork ID zero require removal in Music and reimport.
Native decoded-artwork and cold-reopen QA was tests/ipod/test_media_artwork.py (retired).
Deterministic schema error QA: tests/slice/ipod-media-schema.c.
