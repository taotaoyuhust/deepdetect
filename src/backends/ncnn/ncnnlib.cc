/**
 * DeepDetect
 * Copyright (c) 2018 Jolibrain
 * Author: Corentin Barreau <corentin.barreau@epitech.eu>
 *
 * This file is part of deepdetect.
 *
 * deepdetect is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * deepdetect is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with deepdetect.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "outputconnectorstrategy.h"
#include <thread>
#include <algorithm>
#include "utils/utils.hpp"

// NCNN
#include "ncnnlib.h"
#include "ncnninputconns.h"
#include "cpu.h"
#include "net.h"

namespace dd
{
  template <class TInputConnectorStrategy, class TOutputConnectorStrategy, class TMLModel>
    ncnn::UnlockedPoolAllocator NCNNLib<TInputConnectorStrategy,TOutputConnectorStrategy,TMLModel>::_blob_pool_allocator
      = ncnn::UnlockedPoolAllocator();
    template <class TInputConnectorStrategy, class TOutputConnectorStrategy, class TMLModel>
    ncnn::PoolAllocator NCNNLib<TInputConnectorStrategy,TOutputConnectorStrategy,TMLModel>::_workspace_pool_allocator
      = ncnn::PoolAllocator();
  
    template <class TInputConnectorStrategy, class TOutputConnectorStrategy, class TMLModel>
    NCNNLib<TInputConnectorStrategy,TOutputConnectorStrategy,TMLModel>::NCNNLib(const NCNNModel &cmodel)
        :MLLib<TInputConnectorStrategy,TOutputConnectorStrategy,NCNNModel>(cmodel)
    {
        this->_libname = "ncnn";
        _net = new ncnn::Net();
    }

    template <class TInputConnectorStrategy, class TOutputConnectorStrategy, class TMLModel>
    NCNNLib<TInputConnectorStrategy,TOutputConnectorStrategy,TMLModel>::NCNNLib(NCNNLib &&tl) noexcept
        :MLLib<TInputConnectorStrategy,TOutputConnectorStrategy,NCNNModel>(std::move(tl))
    {
        this->_libname = "ncnn";
	_net = tl._net;
	tl._net = nullptr;
	_nclasses = tl._nclasses;
    _threads = tl._threads;
    }

    template <class TInputConnectorStrategy, class TOutputConnectorStrategy, class TMLModel>
    NCNNLib<TInputConnectorStrategy,TOutputConnectorStrategy,TMLModel>::~NCNNLib()
    {
      delete _net;
      _net = nullptr;
    }

    template <class TInputConnectorStrategy, class TOutputConnectorStrategy, class TMLModel>
    void NCNNLib<TInputConnectorStrategy,TOutputConnectorStrategy,TMLModel>::init_mllib(const APIData &ad)
    {
        _net->load_param(this->_mlmodel._params.c_str());
        _net->load_model(this->_mlmodel._weights.c_str());

        if (ad.has("nclasses"))
	        _nclasses = ad.get("nclasses").get<int>();

        if (ad.has("threads"))
            _threads = ad.get("threads").get<int>();
        else
            _threads = dd_utils::my_hardware_concurrency();

        _blob_pool_allocator.set_size_compare_ratio(0.0f);
        _workspace_pool_allocator.set_size_compare_ratio(0.5f);
        ncnn::Option opt;
        opt.lightmode = true;
        opt.num_threads = _threads;
        opt.blob_allocator = &_blob_pool_allocator;
        opt.workspace_allocator = &_workspace_pool_allocator;
        ncnn::set_default_option(opt);
	model_type(this->_mlmodel._params,this->_mltype);
    }

    template <class TInputConnectorStrategy, class TOutputConnectorStrategy, class TMLModel>
    void NCNNLib<TInputConnectorStrategy,TOutputConnectorStrategy,TMLModel>::clear_mllib(const APIData &ad)
    {
        (void)ad;
    }

    template <class TInputConnectorStrategy, class TOutputConnectorStrategy, class TMLModel>
    int NCNNLib<TInputConnectorStrategy,TOutputConnectorStrategy,TMLModel>::train(const APIData &ad,
                                        APIData &out)
    {
      (void)ad;
      (void)out;
      return 0;
    }

    template <class TInputConnectorStrategy, class TOutputConnectorStrategy, class TMLModel>
    int NCNNLib<TInputConnectorStrategy,TOutputConnectorStrategy,TMLModel>::predict(const APIData &ad,
										APIData &out)
    {
        TInputConnectorStrategy inputc(this->_inputc);
        TOutputConnectorStrategy tout;
        try {
            inputc.transform(ad);
        } catch (...) {
            throw;
        }

        ncnn::Extractor ex = _net->create_extractor();
        ex.set_num_threads(_threads);
        ex.input("data", inputc._in);

        APIData ad_output = ad.getobj("parameters").getobj("output");

        // Get bbox
        bool bbox = false;
        if (ad_output.has("bbox"))
	  bbox = ad_output.get("bbox").get<bool>();

	// Ctc model
	bool ctc = false;
	int blank_label = -1;
	if (ad_output.has("ctc"))
	  {
	    ctc = ad_output.get("ctc").get<bool>();
	    if (ctc)
	      {
		if (ad_output.has("blank_label"))
		  blank_label = ad_output.get("blank_label").get<int>();
	      }
	  }
	
        // Extract detection or classification
        int ret = 0;
	std::string out_blob = "prob";
        if (bbox == true)
	  out_blob = "detection_out";
	else if (ctc == true)
	  out_blob = "probs";
	ret = ex.extract(out_blob.c_str(),inputc._out);
        if (ret == -1) {
            throw MLLibInternalException("NCNN internal error");
        }

        std::vector<APIData> vrad;
        std::vector<double> probs;
        std::vector<std::string> cats;
        std::vector<APIData> bboxes;
        APIData rad;
	
        // Get confidence_threshold
        float confidence_threshold = 0.0;
        if (ad_output.has("confidence_threshold")) {
            apitools::get_float(ad_output, "confidence_threshold", confidence_threshold);
        }

        // Get best
        int best = 1;
        if (ad_output.has("best")) {
            best = ad_output.get("best").get<int>();
        }

        if (bbox == true)
	  {
            for (int i = 0; i < inputc._out.h; i++) {
                const float* values = inputc._out.row(i);
                if (values[1] < confidence_threshold)
                    continue;

                cats.push_back(this->_mlmodel.get_hcorresp(values[0]));
                probs.push_back(values[1]);

                APIData ad_bbox;
                ad_bbox.add("xmin",values[2] * inputc._images_size.at(0).second);
                ad_bbox.add("ymax",values[3] * inputc._images_size.at(0).first);
                ad_bbox.add("xmax",values[4] * inputc._images_size.at(0).second);
                ad_bbox.add("ymin",values[5] * inputc._images_size.at(0).first);
                bboxes.push_back(ad_bbox);
            }
        }
	else if (ctc == true)
	  {
	    int alphabet = inputc._out.w;
	    int time_step = inputc._out.h;
	    std::vector<int> pred_label_seq_with_blank(time_step);
	    for (int t=0;t<time_step;++t)
	      {
		const float *values = inputc._out.row(t);
		pred_label_seq_with_blank[t] = std::distance(values,std::max_element(values,values+alphabet));
	      }

	    std::vector<int> pred_label_seq;
	    int prev = blank_label;
	    for (int t=0;t<time_step;++t)
	      {
		int cur = pred_label_seq_with_blank[t];
		if (cur != prev && cur != blank_label)
		  pred_label_seq.push_back(cur);
		prev = cur;
	      }
	    std::string outstr;
	    std::ostringstream oss;
	    for (auto l: pred_label_seq)
	      outstr += char(std::atoi(this->_mlmodel.get_hcorresp(l).c_str()));
	    cats.push_back(outstr);
	    probs.push_back(1.0);
	  }
	else {
            std::vector<float> cls_scores;

            cls_scores.resize(inputc._out.w);
            for (int j = 0; j < inputc._out.w; j++) {
                cls_scores[j] = inputc._out[j];
            }
            int size = cls_scores.size();
            std::vector< std::pair<float, int> > vec;
            vec.resize(size);
            for (int i = 0; i < size; i++) {
                vec[i] = std::make_pair(cls_scores[i], i);
            }
        
            std::partial_sort(vec.begin(), vec.begin() + best, vec.end(),
                              std::greater< std::pair<float, int> >());
        
            for (int i = 0; i < best; i++)
            {
                if (vec[i].first < confidence_threshold)
                    continue;
                cats.push_back(this->_mlmodel.get_hcorresp(vec[i].second));
                probs.push_back(vec[i].first);
            }
        }

	rad.add("uri",inputc._ids.at(0));
	rad.add("loss", 0.0);
        rad.add("probs", probs);
        rad.add("cats", cats);
        if (bbox == true)
            rad.add("bboxes", bboxes);
        vrad.push_back(rad);
	tout.add_results(vrad);
	out.add("nclasses", this->_nclasses);
        if (bbox == true)
            out.add("bbox", true);
        out.add("roi", false);
        out.add("multibox_rois", false);
	tout.finalize(ad.getobj("parameters").getobj("output"),out,static_cast<MLModel*>(&this->_mlmodel));
        out.add("status", 0);
	return 0;
    }

  template <class TInputConnectorStrategy, class TOutputConnectorStrategy, class TMLModel>
  void NCNNLib<TInputConnectorStrategy,TOutputConnectorStrategy,TMLModel>::model_type(const std::string &param_file,
										     std::string &mltype)
    {
      std::ifstream paramf(param_file);
      std::stringstream content;
      content << paramf.rdbuf();
      
      std::size_t found_detection = content.str().find("DetectionOutput");
      if (found_detection != std::string::npos)
	{
	  mltype = "detection";
	  return;
	}
      std::size_t found_ocr = content.str().find("ContinuationIndicator");
      if (found_ocr != std::string::npos)
	{
	  mltype = "ctc";
	  return;
	}
      mltype = "classification";
    }
  
    template class NCNNLib<ImgNCNNInputFileConn,SupervisedOutput,NCNNModel>;
}
