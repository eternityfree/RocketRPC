## 使用方法
首先拉取源码解压到本地，并执行make操作，再进行安装
```shell
make -j4
sudo make install
```
之后在任意文件夹创建对应的rpc服务的proto文件，比如说order.proto
```proto
syntax = "proto3";
option cc_generic_services = true;
// 支付请求
message payRequest {
  int32 amount = 1; // 支付金额
}

message makeOrderResponse {
  int32 ret_code = 1;
  string res_info = 2;
  string pay_result = 3; 支付结果
}

service Order {
  rpc makeOrder(makeOrderRequest) returns (makeOrderResponse);
}
```
之后再终端输入生成模板的命令
```shell
python3 <源码的rocket_generator.py路径> -i <proto文件路径> -o <生成的框架代码路径>
```
生成的文件夹目录结构如下：
![alt text](image-1.png)
order下的interface下的make_orde.cc文件下的run方法写自己的业务逻辑。