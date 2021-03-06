// Copyright (C) 2019 Storj Labs, Inc.
// See LICENSE for copying information.

#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "require.h"
#include "uplink.h"
#include "helpers.h"

void handle_project(ProjectRef project);

int main(int argc, char *argv[]) {
    with_test_project(&handle_project);
}

void handle_project(ProjectRef project) {
    char *_err = "";
    char **err = &_err;

    char *bucket_name = "test-bucket";

    uint8_t *salted_key = project_salted_key_from_passphrase(project,
                                                             "It's dangerous to go alone, take this!",
                                                             err);
    require_noerror(*err);

    EncryptionAccessRef encryption_access = new_encryption_access_with_default_key(salted_key);
    char *enc_ctx = serialize_encryption_access(encryption_access, err);
    require_noerror(*err);

    char *canceled_object_path = "test-object-canceled";
    char *object_paths[] = {"test-object1","test-object2","test-object3","test-object4"};
    int num_of_objects = 4;

    // NB: about +500 years from time of writing
    int64_t future_expiration_timestamp = 17329017831;

    { // create buckets
        BucketConfig config = test_bucket_config();
        BucketInfo info = create_bucket(project, bucket_name, &config, err);
        require_noerror(*err);
        free_bucket_info(&info);
    }

    // open bucket
    BucketRef bucket = open_bucket(project, bucket_name, enc_ctx, err);
    require_noerror(*err);

    UploadOptions opts = {
        "text/plain",
        future_expiration_timestamp,
    };

    { // upload cancel
        // NB: 5KB
        size_t data_len = 1024 * 5;
        uint8_t *data = malloc(data_len);
        fill_random_data(data, data_len);
        UploaderRef uploader = upload(bucket, canceled_object_path, &opts, err);
        require_noerror(*err);

        size_t uploaded_total = 0;
        for (int i = 0; i < 3; i++) {
            size_t size_to_write = (data_len - uploaded_total > 256) ? 256 : data_len - uploaded_total;

            if (size_to_write == 0) {
                break;
            }

            size_t write_size = upload_write(uploader, (uint8_t *)data+uploaded_total, size_to_write, err);
            require_noerror(*err);

            if (write_size == 0) {
                break;
            }

            uploaded_total += write_size;
        }
        require(uploaded_total > 0);

        upload_cancel(uploader, err);
        require_noerror(*err);

        {
            // canceled upload shouldn't be listed
            ObjectList object_list = list_objects(bucket, NULL, err);
            require(object_list.length == 0);

            // canceling canceled upload isn't an error
            upload_cancel(uploader, err);
            require_noerror(*err);

            // committing canceled upload isn't an error
            upload_commit(uploader, err);
            require_noerror(*err);

            // writing canceled upload isn't an error
            int write_size = upload_write(uploader, (uint8_t *)data, data_len, err);
            require_noerror(*err);
            require(write_size == 0);
        }

        free_uploader(uploader);
    }

    for(int i = 0; i < num_of_objects; i++) {
        // NB: 5KB, 50KB, 500KB, 5000KB
        size_t data_len = pow(10, (double)i) * 1024 * 5;
        uint8_t *data = malloc(data_len);
        fill_random_data(data, data_len);

        { // upload

            UploaderRef uploader = upload(bucket, object_paths[i], &opts, err);
            require_noerror(*err);

            size_t uploaded_total = 0;
            while (uploaded_total < data_len) {
                size_t size_to_write = (data_len - uploaded_total > 256) ? 256 : data_len - uploaded_total;

                if (size_to_write == 0) {
                    break;
                }

                size_t write_size = upload_write(uploader, (uint8_t *)data+uploaded_total, size_to_write, err);
                require_noerror(*err);

                if (write_size == 0) {
                    break;
                }

                uploaded_total += write_size;
            }

            upload_commit(uploader, err);
            require_noerror(*err);

            free_uploader(uploader);
        }

        { // object meta
            ObjectRef object_ref = open_object(bucket, object_paths[i], err);
            require_noerror(*err);

            ObjectMeta object_meta = get_object_meta(object_ref, err);
            require_noerror(*err);
            require(strcmp(object_paths[i], object_meta.path) == 0);
            require(data_len == object_meta.size);
            require(future_expiration_timestamp == object_meta.expires);
            require((time(NULL) - object_meta.created) <= 2);
            require((time(NULL) - object_meta.modified) <= 2);
            require(object_meta.checksum_bytes != NULL);
            // TODO: checksum is an empty slice in go; is that expected?
            // require(object_meta.checksum_length != 0);

            free_object_meta(&object_meta);
            close_object(object_ref, err);
        }

        { // download
            DownloaderRef downloader = download(bucket, object_paths[i], err);
            require_noerror(*err);

            uint8_t *downloaded_data = malloc(data_len);
            memset(downloaded_data, '\0', data_len);
            size_t downloaded_total = 0;

            size_t size_to_read = 256 + i;
            while (downloaded_total < data_len) {
                size_t read_size = download_read(downloader, &downloaded_data[downloaded_total], size_to_read, err);
                require_noerror(*err);

                if (read_size == 0) {
                    break;
                }

                downloaded_total += read_size;
            }

            download_close(downloader, err);
            require_noerror(*err);
            require(memcmp(data, downloaded_data, data_len) == 0);

            free(downloaded_data);
            free_downloader(downloader);
        }

        { // download range
            long start = 100 + (i+1);
            long limit = 1024 * (i+1);
            DownloaderRef downloader = download_range(bucket, object_paths[i], start, limit, err);
            require_noerror(*err);

            uint8_t *downloaded_data = malloc(limit);
            memset(downloaded_data, '\0', limit);
            size_t downloaded_total = 0;

            size_t size_to_read = 256 + i;
            while (downloaded_total < limit) {
                size_t read_size = download_read(downloader, &downloaded_data[downloaded_total], size_to_read, err);
                require_noerror(*err);

                if (read_size == 0) {
                    break;
                }

                downloaded_total += read_size;
            }

            download_close(downloader, err);
            require_noerror(*err);
            require(limit == downloaded_total);
            require(memcmp(&data[start], downloaded_data, limit) == 0);

            free(downloaded_data);
            free_downloader(downloader);
        }

        if (data != NULL) {
            free(data);
        }

        require_noerror(*err);
    }

    { // download cancellation
        // NB: 5KB
        size_t data_len = 1024 * 5;
        DownloaderRef downloader = download(bucket, object_paths[0], err);
        require_noerror(*err);

        uint8_t *downloaded_data = malloc(data_len);
        memset(downloaded_data, '\0', data_len);
        size_t downloaded_total = 0;

        size_t size_to_read = 256;
        for (int i = 0; i < 3; i++) {
            size_t read_size = download_read(downloader, &downloaded_data[downloaded_total], size_to_read, err);
            require_noerror(*err);

            if (read_size == 0) {
                break;
            }

            downloaded_total += read_size;
        }
        require(downloaded_total > 0);

        download_cancel(downloader, err);
        require_noerror(*err);

        {
            // canceling canceled download isn't an error
            download_cancel(downloader, err);
            require_noerror(*err);

            // closing canceled download isn't an error
            download_close(downloader, err);
            require_noerror(*err);

            // writing canceled download isn't an error
            int read_size = download_read(downloader, (uint8_t *)downloaded_data, data_len, err);
            require_noerror(*err);
            require(read_size == 0);
        }

        free(downloaded_data);
        free_downloader(downloader);
    }

    { // List objects
        ObjectList objects_list = list_objects(bucket, NULL, err);
        require_noerror(*err);
        require(strcmp(bucket_name, objects_list.bucket) == 0);
        require(strcmp("", objects_list.prefix) == 0);
        require(false == objects_list.more);
        require(num_of_objects == objects_list.length);

        ObjectInfo *object;
        for (int i=0; i < objects_list.length; i++) {
            object = &objects_list.items[i];
            const char *objNumStr = strtok(object->path, "test-object");
            int objNum = atoi(objNumStr);
            // NB: 5KB, 50KB, 500KB, 5000KB
            size_t expected_size = pow(10, (double)objNum-1) * 1024 * 5;
            require(expected_size == object->size);

            require(array_contains(object->path, object_paths, num_of_objects));
        }

        free_list_objects(&objects_list);
    }

    { // Delete objects
        for (int i = 0; i < num_of_objects; i++) {
            delete_object(bucket, object_paths[i], err);
            require_noerror(*err);

            // ensure object deletion
            ObjectList objects_list = list_objects(bucket, NULL, err);
            require_noerror(*err);
            require(objects_list.items);
            require(objects_list.length == num_of_objects - i - 1);

            free_list_objects(&objects_list);
        }
    }

    close_bucket(bucket, err);
    require_noerror(*err);

    free_encryption_access(encryption_access);
    free(salted_key);
}
