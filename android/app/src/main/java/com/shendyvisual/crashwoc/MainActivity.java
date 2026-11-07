package com.shendyvisual.crashwoc;

import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.widget.Toast;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** SDL3 Android host activity with SAF-based, app-private disc importing. */
public final class MainActivity extends SDLActivity {
    private static final int REQUEST_DISC_FILE = 7301;
    private static final int REQUEST_DISC_FOLDER = 7302;
    private static final ExecutorService DISC_IMPORT_EXECUTOR = Executors.newSingleThreadExecutor();
    private String discImportTarget;

    /** Called by the native setup wizard. No broad storage permission is required. */
    public void openDiscPicker(String targetDir, boolean folderMode) {
        discImportTarget = targetDir;
        Intent intent;
        int request;
        if (folderMode) {
            intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
            request = REQUEST_DISC_FOLDER;
        } else {
            intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("*/*");
            intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {
                "application/octet-stream", "application/x-iso9660-image", "application/x-cue"
            });
            request = REQUEST_DISC_FILE;
        }
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        if (folderMode) {
            intent.addFlags(Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        }
        try {
            startActivityForResult(intent, request);
        } catch (RuntimeException error) {
            showToast("Cannot open Android file picker: " + error.getMessage());
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if ((requestCode != REQUEST_DISC_FILE && requestCode != REQUEST_DISC_FOLDER)
                || resultCode != RESULT_OK || data == null || data.getData() == null) {
            return;
        }

        final Uri uri = data.getData();
        final String target = discImportTarget;
        if (target == null || target.isEmpty()) {
            showToast("Import folder is unavailable.");
            return;
        }

        if (requestCode == REQUEST_DISC_FOLDER) {
            try {
                getContentResolver().takePersistableUriPermission(
                    uri, data.getFlags() & Intent.FLAG_GRANT_READ_URI_PERMISSION);
            } catch (SecurityException ignored) {
                // The current copy is still allowed by the temporary picker grant.
            }
            showToast("Copying BIN/CUE files into the game folder...");
            DISC_IMPORT_EXECUTOR.execute(() -> copyDiscFolder(uri, target));
        } else {
            showToast("Copying disc image into the game folder...");
            DISC_IMPORT_EXECUTOR.execute(() -> copySingleDisc(uri, target));
        }
    }

    private void copySingleDisc(Uri uri, String targetPath) {
        String name = queryDisplayName(uri);
        if (!isDiscImage(name)) {
            showToast("Choose an .iso image, or use the folder option for .bin/.cue.");
            return;
        }
        File targetDir = new File(targetPath);
        if (!targetDir.exists() && !targetDir.mkdirs()) {
            showToast("Cannot create the private import folder.");
            return;
        }
        File destination = new File(targetDir, safeName(name));
        if (copyUri(uri, destination)) {
            showToast("Disc image copied. Select Import in the setup screen.");
        }
    }

    private void copyDiscFolder(Uri treeUri, String targetPath) {
        File targetDir = new File(targetPath);
        if (!targetDir.exists() && !targetDir.mkdirs()) {
            showToast("Cannot create the private import folder.");
            return;
        }

        int copied = 0;
        String treeId = DocumentsContract.getTreeDocumentId(treeUri);
        Uri childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, treeId);
        String[] projection = {
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME
        };
        try (Cursor cursor = getContentResolver().query(childrenUri, projection, null, null, null)) {
            if (cursor == null) {
                showToast("Cannot read the selected folder.");
                return;
            }
            int idColumn = cursor.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_DOCUMENT_ID);
            int nameColumn = cursor.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_DISPLAY_NAME);
            while (cursor.moveToNext()) {
                String documentId = cursor.getString(idColumn);
                String name = cursor.getString(nameColumn);
                if (!isDiscImage(name)) {
                    continue;
                }
                Uri child = DocumentsContract.buildDocumentUriUsingTree(treeUri, documentId);
                if (copyUri(child, new File(targetDir, safeName(name)))) {
                    copied++;
                }
            }
        } catch (Exception error) {
            showToast("Folder import failed: " + error.getMessage());
            return;
        }
        showToast(copied > 0
            ? "Copied " + copied + " disc file(s). Select the .cue file to import."
            : "No .bin, .cue, or .iso files found in that folder.");
    }

    private boolean copyUri(Uri uri, File destination) {
        File partial = new File(destination.getAbsolutePath() + ".partial");
        try (InputStream input = getContentResolver().openInputStream(uri);
             FileOutputStream output = new FileOutputStream(partial)) {
            if (input == null) {
                throw new IllegalStateException("The selected document cannot be opened.");
            }
            byte[] buffer = new byte[1024 * 1024];
            int count;
            while ((count = input.read(buffer)) != -1) {
                output.write(buffer, 0, count);
            }
            output.getFD().sync();
            if (destination.exists() && !destination.delete()) {
                throw new IllegalStateException("Cannot replace existing import file.");
            }
            if (!partial.renameTo(destination)) {
                throw new IllegalStateException("Cannot finish copying the disc image.");
            }
            return true;
        } catch (Exception error) {
            // A partial copy is never exposed as a valid image to the native scanner.
            //noinspection ResultOfMethodCallIgnored
            partial.delete();
            showToast("Disc copy failed: " + error.getMessage());
            return false;
        }
    }

    private String queryDisplayName(Uri uri) {
        try (Cursor cursor = getContentResolver().query(uri, null, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int index = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (index >= 0) {
                    return cursor.getString(index);
                }
            }
        } catch (Exception ignored) {
            // Fall back to the last URI path segment.
        }
        String segment = uri.getLastPathSegment();
        return segment == null ? "disc.iso" : segment;
    }

    private static boolean isDiscImage(String name) {
        if (name == null) return false;
        String lower = name.toLowerCase(Locale.ROOT);
        return lower.endsWith(".iso") || lower.endsWith(".bin") || lower.endsWith(".cue");
    }

    private static String safeName(String name) {
        String safe = name == null ? "disc.iso" : name.replaceAll("[\\\\/:*?\"<>|]", "_");
        return safe.isEmpty() || safe.startsWith(".") ? "disc.iso" : safe;
    }

    private void showToast(String message) {
        runOnUiThread(() -> Toast.makeText(this, message, Toast.LENGTH_LONG).show());
    }
}
