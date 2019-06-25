let k_update_timer_fun = null;
const test_app = new Vue({
    el: '#el_test',
    data: {
        progres1 : 20,
        progres2 : 100,
        progres3 : 40,
        progres4 : 50,
        switch_on : false,
        file : null,
        loadingStatus : false,
    },
    methods: {
        handlButtonClick() {
            if(k_update_timer_fun == null){
                    k_update_timer_fun = setInterval( function()  {
                    test_app.progres1 = (test_app.progres1 + 1) % 100;
                    test_app.progres2 = (test_app.progres2 + 2) % 100;
                    test_app.progres3 = (test_app.progres3 + 3) % 100;
                    test_app.progres4 = (test_app.progres4 + 4) % 100;
                }, 100);
            }else {
                clearInterval(k_update_timer_fun);
                k_update_timer_fun = null;
            }
        },
        format(percentage) {
            return percentage === 100 ? '满' : `${percentage}%`;
        },
        //	:before-upload="handleUpload"
        handleUpload (file) {
            this.$Message.success('上传测试');
            this.file = file;
            return false;
        },
        upload () {
            this.loadingStatus = true;
            setTimeout(() => {
                this.file = null;
                this.loadingStatus = false;
                this.$Message.success('Success')
            }, 1500);
        },
        handleUploadSuccess(error, file, fileList){
            this.$Message.success('上传成功');
        },
        handleUploadError(error, file, fileList){
            this.$Message.error('上传失败');
        }
    }
    //render: h => h(App)
  });