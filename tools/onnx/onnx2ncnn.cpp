// Tencent is pleased to support the open source community by making ncnn available.
//
// Copyright (C) 2017 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
// https://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#include <stdio.h>
#include <limits.h>

#include <iostream>

#include <fstream>
#include <set>
#include <limits>
#include <algorithm>

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/text_format.h>
#include <google/protobuf/message.h>

#include "onnx.pb.h"

static bool read_proto_from_binary(const char* filepath, google::protobuf::Message* message)
{
    std::ifstream fs(filepath, std::ifstream::in | std::ifstream::binary);
    if (!fs.is_open())
    {
        fprintf(stderr, "open failed %s\n", filepath);
        return false;
    }

    google::protobuf::io::IstreamInputStream input(&fs);
    google::protobuf::io::CodedInputStream codedstr(&input);

    codedstr.SetTotalBytesLimit(INT_MAX, INT_MAX / 2);

    bool success = message->ParseFromCodedStream(&codedstr);

    fs.close();

    return success;
}

static std::vector<int> get_node_attr_ai(const onnx::NodeProto& node, const char* key)
{
    std::vector<int> v;

    for (int i=0; i<node.attribute_size(); i++)
    {
        const onnx::AttributeProto& attr = node.attribute(i);
        if (attr.name() == key)
        {
            v.resize(attr.ints_size());
            for (int j=0; j<attr.ints_size(); j++)
            {
                v[j] = attr.ints(j);
            }

            break;
        }
    }

    return v;
}

static int get_node_attr_i(const onnx::NodeProto& node, const char* key, int def = 0)
{
    for (int i=0; i<node.attribute_size(); i++)
    {
        const onnx::AttributeProto& attr = node.attribute(i);
        if (attr.name() == key)
        {
            return attr.i();
        }
    }

    return def;
}

static float get_node_attr_f(const onnx::NodeProto& node, const char* key, float def = 0.f)
{
    for (int i=0; i<node.attribute_size(); i++)
    {
        const onnx::AttributeProto& attr = node.attribute(i);
        if (attr.name() == key)
        {
            return attr.f();
        }
    }

    return def;
}

static int get_tensor_proto_data_size(const onnx::TensorProto& tp)
{
    if (tp.has_raw_data())
    {
        const std::string& raw_data = tp.raw_data();
        int size = (int)raw_data.size() / 4;
        return size;
    }
    else if (tp.data_type() == 1)
    {
        return tp.float_data_size();
    }

    return 0;
}

static void fwrite_tensor_proto_data(const onnx::TensorProto& tp, FILE* bp)
{
    int size = get_tensor_proto_data_size(tp);

    if (tp.has_raw_data())
    {
        const std::string& raw_data = tp.raw_data();
        fwrite(raw_data.data(), sizeof(float), size, bp);
    }
    else if (tp.data_type() == 1)
    {
        fwrite(tp.float_data().data(), sizeof(float), size, bp);
    }
}

int main(int argc, char** argv)
{
    const char* onnxpb = argv[1];
    const char* ncnn_prototxt = argc >= 4 ? argv[2] : "ncnn.param";
    const char* ncnn_modelbin = argc >= 4 ? argv[3] : "ncnn.bin";

    onnx::ModelProto model;

    // load
    bool s1 = read_proto_from_binary(onnxpb, &model);
    if (!s1)
    {
        fprintf(stderr, "read_proto_from_binary failed\n");
        return -1;
    }

    FILE* pp = fopen(ncnn_prototxt, "wb");
    FILE* bp = fopen(ncnn_modelbin, "wb");

    // magic
    fprintf(pp, "7767517\n");

    const onnx::GraphProto& graph = model.graph();

    int node_count = graph.node_size();

    // node reference
    std::map<std::string, int> node_reference;

    // weight node and weight reshape node
    std::map<std::string, int> weight_nodes;

    for (int j=0; j<graph.initializer_size(); j++)
    {
        const onnx::TensorProto& initializer = graph.initializer(j);

        weight_nodes[initializer.name()] = j;
    }

    // global definition line
    // [layer count] [blob count]
    std::set<std::string> blob_names;
    for (int i=0; i<node_count; i++)
    {
        const onnx::NodeProto& node = graph.node(i);

        const std::string& op = node.op_type();

        std::string name = node.name();
        if (name.empty())
        {
            name = node.output(0);
        }

        if (op == "Reshape")
        {
            if (node.input_size() == 1)
            {
                const std::string& input_name = node.input(0);

                // check weight
                if (weight_nodes.find(input_name) != weight_nodes.end())
                {
                    weight_nodes[name] = weight_nodes[input_name];
                    continue;
                }
            }
        }

        for (int j=0; j<(int)node.input_size(); j++)
        {
            const std::string& input_name = node.input(j);

            // check weight
            if (weight_nodes.find(input_name) != weight_nodes.end())
            {
                continue;
            }

            blob_names.insert(input_name);

            if (node_reference.find(input_name) == node_reference.end())
            {
                node_reference[input_name] = 1;
            }
            else
            {
                node_reference[input_name] = node_reference[input_name] + 1;
            }
        }

        for (int j=0; j<(int)node.output_size(); j++)
        {
            const std::string& output_name = node.output(j);

            blob_names.insert(output_name);
        }
    }

    // include Input node
    int input_node_count = 0;
    for (int j=0; j<graph.input_size(); j++)
    {
        const std::string& input_name = graph.input(j).name();

        // check weight
        if (weight_nodes.find(input_name) != weight_nodes.end())
            continue;

        blob_names.insert(input_name);

        input_node_count++;
    }

    // remove node_reference entry with reference equals to one
    int splitncnn_blob_count = 0;
    std::map<std::string, int>::iterator it = node_reference.begin();
    while (it != node_reference.end())
    {
        if (it->second == 1)
        {
            node_reference.erase(it++);
        }
        else
        {
            splitncnn_blob_count += it->second;
//             fprintf(stderr, "%s %d\n", it->first.c_str(), it->second);
            ++it;
        }
    }

    fprintf(pp, "%lu %lu\n", node_count + input_node_count + node_reference.size() + graph.initializer_size() - weight_nodes.size(), blob_names.size() + splitncnn_blob_count);

    int internal_split = 0;

    // place Input at the beginning
    for (int j=0; j<graph.input_size(); j++)
    {
        const std::string& input_name = graph.input(j).name();

        // check weight
        if (weight_nodes.find(input_name) != weight_nodes.end())
            continue;

        fprintf(pp, "%-16s %-24s 0 1 %s\n", "Input", input_name.c_str(), input_name.c_str());
    }

    for (int i=0; i<node_count; i++)
    {
        const onnx::NodeProto& node = graph.node(i);

        const std::string& op = node.op_type();

        std::string name = node.name();
        if (name.empty())
        {
            name = node.output(0);
        }

        int input_size = node.input_size();
        int output_size = node.output_size();

        for (int j=0; j<(int)node.input_size(); j++)
        {
            const std::string& input_name = node.input(j);

            // check weight
            if (weight_nodes.find(input_name) != weight_nodes.end())
            {
                input_size--;
            }

//             fprintf(stderr, "  input = %s\n", input_name.c_str());
        }

        for (int j=0; j<(int)node.output_size(); j++)
        {
            const std::string& output_name = node.output(j);

//             fprintf(stderr, "  output = %s\n", output_name.c_str());
        }

        if (op == "AveragePool" || op == "MaxPool")
        {
            fprintf(pp, "%-16s", "Pooling");
        }
        else if (op == "BatchNormalization")
        {
            fprintf(pp, "%-16s", "BatchNorm");
        }
        else if (op == "Concat")
        {
            fprintf(pp, "%-16s", "Concat");
        }
        else if (op == "Conv")
        {
            int group = get_node_attr_i(node, "group", 1);
            if (group > 1) {
                fprintf(pp, "%-16s", "ConvolutionDepthWise");
            } else {
                fprintf(pp, "%-16s", "Convolution");
            }
        }
        else if (op == "Dropout")
        {
            fprintf(pp, "%-16s", "Dropout");
        }
        else if (op == "Gemm")
        {
            fprintf(pp, "%-16s", "Innerproduct");
        }
        else if (op == "GlobalAveragePool")
        {
            fprintf(pp, "%-16s", "Pooling");
        }
        else if (op == "GlobalMaxPool")
        {
            fprintf(pp, "%-16s", "Pooling");
        }
        else if (op == "LRN")
        {
            fprintf(pp, "%-16s", "LRN");
        }
        else if (op == "Relu")
        {
            fprintf(pp, "%-16s", "ReLU");
        }
        else if (op == "Reshape")
        {
            if (node.input_size() == 1)
            {
                const std::string& input_name = node.input(0);

                // skip weight reshape
                if (weight_nodes.find(input_name) != weight_nodes.end())
                {
                    weight_nodes[name] = weight_nodes[input_name];
                    continue;
                }
            }
            fprintf(pp, "%-16s", "Reshape");
        }
        else if (op == "Softmax")
        {
            fprintf(pp, "%-16s", "Softmax");
        }
        else if (op == "Transpose")
        {
            fprintf(pp, "%-16s", "Permute");
        }
        else
        {
            // TODO
            fprintf(pp, "%-16s", op.c_str());
        }

        fprintf(pp, " %-24s %d %d", name.c_str(), input_size, output_size);

        for (int j=0; j<node.input_size(); j++)
        {
            std::string input_name = node.input(j);

            // check weight
            if (weight_nodes.find(input_name) != weight_nodes.end())
            {
                continue;
            }

            if (node_reference.find(input_name) != node_reference.end())
            {
                int refidx = node_reference[input_name] - 1;
                node_reference[input_name] = refidx;

                char splitsuffix[256];
                sprintf(splitsuffix, "_splitncnn_%d", refidx);
                input_name = input_name + splitsuffix;
            }

            fprintf(pp, " %s", input_name.c_str());
        }

        for (int j=0; j<node.output_size(); j++)
        {
            const std::string& output_name = node.output(j);

            fprintf(pp, " %s", output_name.c_str());
        }

        if (op == "AveragePool" || op == "MaxPool")
        {
            std::vector<int> kernel_shape = get_node_attr_ai(node, "kernel_shape");
            std::vector<int> strides = get_node_attr_ai(node, "strides");
            std::vector<int> pads = get_node_attr_ai(node, "pads");

            int pool = op == "AveragePool" ? 1 : 0;
            int pad_mode = 1;

            fprintf(pp, " 0=%d", pool);

            if (kernel_shape.size() == 1) {
                fprintf(pp, " 1=%d", kernel_shape[0]);
            } else if (kernel_shape.size() == 2) {
                fprintf(pp, " 1=%d", kernel_shape[1]);
                fprintf(pp, " 11=%d", kernel_shape[0]);
            }

            if (strides.size() == 1) {
                fprintf(pp, " 2=%d", strides[0]);
            } else if (strides.size() == 2) {
                fprintf(pp, " 2=%d", strides[1]);
                fprintf(pp, " 12=%d", strides[0]);
            }

            if (pads.size() == 1) {
                fprintf(pp, " 3=%d", pads[0]);
            } else if (pads.size() == 2) {
                fprintf(pp, " 3=%d", pads[1]);
                fprintf(pp, " 13=%d", pads[0]);
            } else if (pads.size() == 4) {
                fprintf(pp, " 3=%d", pads[1]);
                fprintf(pp, " 13=%d", pads[0]);
                // TODO hpad2=pads[2]   wpad2=pads[3]
            }

            fprintf(pp, " 5=%d", pad_mode);
        }
        else if (op == "BatchNormalization")
        {
            float epsilon = get_node_attr_f(node, "epsilon", 1e-5f);

            const onnx::TensorProto& scale = graph.initializer(weight_nodes[node.input(1)]);
            const onnx::TensorProto& B = graph.initializer(weight_nodes[node.input(2)]);
            const onnx::TensorProto& mean = graph.initializer(weight_nodes[node.input(3)]);
            const onnx::TensorProto& var = graph.initializer(weight_nodes[node.input(4)]);

            fprintf(pp, " 0=%d", get_tensor_proto_data_size(scale));

            fwrite_tensor_proto_data(scale, bp);
            fwrite_tensor_proto_data(mean, bp);
            // apply epsilon to var
            {
                int size = get_tensor_proto_data_size(var);

                const float* v = var.has_raw_data() ? (const float*)var.raw_data().data() : var.float_data().data();

                for (int j=0; j<size; j++)
                {
                    float ve = v[j] + epsilon;
                    fwrite(&ve, sizeof(float), 1, bp);
                }
            }
            fwrite_tensor_proto_data(B, bp);
        }
        else if (op == "Concat")
        {
            int axis = get_node_attr_i(node, "axis", 1);
            fprintf(pp, " 0=%d", axis-1);
        }
        else if (op == "Conv")
        {
            const onnx::TensorProto& W = graph.initializer(weight_nodes[node.input(1)]);

            int num_filter = W.dims(0);
            int has_bias = node.input_size() == 3 ? 1 : 0;

            std::vector<int> kernel_shape = get_node_attr_ai(node, "kernel_shape");
            std::vector<int> dilations = get_node_attr_ai(node, "dilations");
            std::vector<int> strides = get_node_attr_ai(node, "strides");
            std::vector<int> pads = get_node_attr_ai(node, "pads");
            int group = get_node_attr_i(node, "group", 1);

            fprintf(pp, " 0=%d", num_filter);

            if (kernel_shape.size() == 1) {
                fprintf(pp, " 1=%d", kernel_shape[0]);
            } else if (kernel_shape.size() == 2) {
                fprintf(pp, " 1=%d", kernel_shape[1]);
                fprintf(pp, " 11=%d", kernel_shape[0]);
            }

            if (dilations.size() == 1) {
                fprintf(pp, " 2=%d", dilations[0]);
            } else if (dilations.size() == 2) {
                fprintf(pp, " 2=%d", dilations[1]);
                fprintf(pp, " 12=%d", dilations[0]);
            }

            if (strides.size() == 1) {
                fprintf(pp, " 3=%d", strides[0]);
            } else if (strides.size() == 2) {
                fprintf(pp, " 3=%d", strides[1]);
                fprintf(pp, " 13=%d", strides[0]);
            }

            if (pads.size() == 1) {
                fprintf(pp, " 4=%d", pads[0]);
            } else if (pads.size() == 2) {
                fprintf(pp, " 4=%d", pads[1]);
                fprintf(pp, " 14=%d", pads[0]);
            } else if (pads.size() == 4) {
                fprintf(pp, " 4=%d", pads[1]);
                fprintf(pp, " 14=%d", pads[0]);
                // TODO hpad2=pads[2]   wpad2=pads[3]
            }

            fprintf(pp, " 5=%d", has_bias);

            fprintf(pp, " 6=%d", get_tensor_proto_data_size(W));

            if (group > 1) {
                fprintf(pp, " 7=%d", group);
            }

            int quantize_tag = 0;
            fwrite(&quantize_tag, sizeof(int), 1, bp);

            fwrite_tensor_proto_data(W, bp);

            if (has_bias)
            {
                const onnx::TensorProto& B = graph.initializer(weight_nodes[node.input(2)]);
                fwrite_tensor_proto_data(B, bp);
            }
        }
        else if (op == "Dropout")
        {
            // no-op
        }
        else if (op == "Gemm")
        {
            float alpha = get_node_attr_f(node, "alpha", 1.f);
            float beta = get_node_attr_f(node, "beta", 1.f);
            int broadcast = get_node_attr_i(node, "broadcast", 0);
            int transA = get_node_attr_i(node, "transA", 0);
            int transB = get_node_attr_i(node, "transB", 0);

            if (alpha == 1.f && beta == 1.f)
            {
                // A * B + C
                if (transA == 0 && transB == 1 && broadcast == 1)
                {
                    const onnx::TensorProto& B = graph.initializer(weight_nodes[node.input(1)]);
                    const onnx::TensorProto& C = graph.initializer(weight_nodes[node.input(2)]);

                    fwrite_tensor_proto_data(B, bp);
                    fwrite_tensor_proto_data(C, bp);
                }
            }
        }
        else if (op == "GlobalAveragePool")
        {
            int pool = 1;
            int global_pool = 1;

            fprintf(pp, " 0=%d", pool);
            fprintf(pp, " 4=%d", global_pool);
        }
        else if (op == "GlobalMaxPool")
        {
            int pool = 0;
            int global_pool = 1;

            fprintf(pp, " 0=%d", pool);
            fprintf(pp, " 4=%d", global_pool);
        }
        else if (op == "LRN")
        {
            float alpha = get_node_attr_f(node, "alpha", 1.f);
            float beta = get_node_attr_f(node, "beta", 0.5f);
            float bias = get_node_attr_f(node, "bias", 1.f);// TODO
            int size = get_node_attr_i(node, "size", 1);

            int norm_region = 0;

            fprintf(pp, " 0=%d", norm_region);
            fprintf(pp, " 1=%d", size);
            fprintf(pp, " 2=%f", alpha);
            fprintf(pp, " 3=%f", beta);
        }
        else if (op == "Reshape")
        {
            std::vector<int> shape = get_node_attr_ai(node, "shape");

            if (shape.size() == 1) {
                fprintf(pp, " 0=%d", shape[0]);// should never reach here
            } else if (shape.size() == 2) {
                fprintf(pp, " 0=%d", shape[1]);
            } else if (shape.size() == 3) {
                fprintf(pp, " 0=%d", shape[2]);
                fprintf(pp, " 1=%d", shape[1]);
            } else if (shape.size() == 4) {
                fprintf(pp, " 0=%d", shape[3]);
                fprintf(pp, " 1=%d", shape[2]);
                fprintf(pp, " 2=%d", shape[1]);
            } else if (shape.size() == 5) {
                fprintf(pp, " 0=%d", shape[4] * shape[3]);
                fprintf(pp, " 1=%d", shape[2]);
                fprintf(pp, " 2=%d", shape[1]);
            }
        }
        else if (op == "Softmax")
        {
            int axis = get_node_attr_i(node, "axis", 1);
            fprintf(pp, " 0=%d", axis-1);
        }
        else if (op == "Transpose")
        {
            std::vector<int> perm = get_node_attr_ai(node, "perm");

            if (perm.size() == 4) {
                if (perm[1] == 1 && perm[2] == 2 && perm[3] == 3)
                    fprintf(pp, " 0=0");// w h c
                else if (perm[1] == 1 && perm[2] == 3 && perm[3] == 2)
                    fprintf(pp, " 0=1");// h w c
                else if (perm[1] == 2 && perm[2] == 1 && perm[3] == 3)
                    fprintf(pp, " 0=2");// w c h
                else if (perm[1] == 2 && perm[2] == 3 && perm[3] == 1)
                    fprintf(pp, " 0=3");// c w h
                else if (perm[1] == 3 && perm[2] == 1 && perm[3] == 2)
                    fprintf(pp, " 0=4");// h c w
                else if (perm[1] == 3 && perm[2] == 2 && perm[3] == 1)
                    fprintf(pp, " 0=5");// c h w
            } else if (perm.size() == 5) {
                if (perm[1] == 1 && perm[2] == 2 && perm[3] == 3 && perm[4] == 4)
                    fprintf(pp, " 0=0");// wx h c
                else if (perm[1] == 1 && perm[2] == 3 && perm[3] == 4 && perm[4] == 2)
                    fprintf(pp, " 0=1");// h wx c
                else if (perm[1] == 2 && perm[2] == 1 && perm[3] == 3 && perm[4] == 4)
                    fprintf(pp, " 0=2");// wx c h
                else if (perm[1] == 2 && perm[2] == 3 && perm[3] == 4 && perm[4] == 1)
                    fprintf(pp, " 0=3");// c wx h
                else if (perm[1] == 3 && perm[2] == 4 && perm[3] == 1 && perm[4] == 2)
                    fprintf(pp, " 0=4");// h c wx
                else if (perm[1] == 3 && perm[2] == 4 && perm[3] == 2 && perm[4] == 1)
                    fprintf(pp, " 0=5");// c h wx
                else
                    fprintf(stderr, "Unsupported transpose type !\n");
            }
        }
        else
        {
            // TODO op specific param
            for (int j=0; j<node.attribute_size(); j++)
            {
                const onnx::AttributeProto& attr = node.attribute(j);
                if (attr.type() == 1)
                {
                    fprintf(stderr, "  # %s=%f\n", attr.name().c_str(), attr.f());
                }
                else if (attr.type() == 2)
                {
                    fprintf(stderr, "  # %s=%d\n", attr.name().c_str(), attr.i());
                }
                else if (attr.type() == 3)
                {
                    fprintf(stderr, "  # %s=%s\n", attr.name().c_str(), attr.s().c_str());
                }
                else
                {
                    fprintf(stderr, "  # %s %d\n", attr.name().c_str(), attr.type());
                }
            }
        }

        fprintf(pp, "\n");

        for (int j=0; j<output_size; j++)
        {
            const std::string& output_name = node.output(j);
            if (node_reference.find(output_name) != node_reference.end())
            {
                int refcount = node_reference[output_name];
                if (refcount > 1)
                {
                    char splitname[256];
                    sprintf(splitname, "splitncnn_%d", internal_split);
                    fprintf(pp, "%-16s %-24s %d %d", "Split", splitname, 1, refcount);

                    fprintf(pp, " %s", output_name.c_str());

                    for (int k=0; k<refcount; k++)
                    {
                        fprintf(pp, " %s_splitncnn_%d", output_name.c_str(), k);
                    }
                    fprintf(pp, "\n");

                    internal_split++;
                }
            }
        }
    }

    fclose(pp);
    fclose(bp);

    return 0;
}
