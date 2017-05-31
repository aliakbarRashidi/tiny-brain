/*
The MIT License

Copyright (c) 2017-2017 Albert Murienne

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include "ocr.h"

//#include "edge_detect.h"

#include "tiny_brain/tinymage.h"

#include <iostream>

using t_digit_interval = std::pair<size_t,size_t>;

tinymage<float> get_cropped_numbers( const tinymage<float>& input );
void compute_ranges( const tinymage<float>& input, std::vector<t_digit_interval>& number_intervals );
void center_number( tinymage<float>& input );

using namespace tiny_dnn;

class ocr_helper::ocr_helper_impl
{
public:
    ocr_helper_impl( network<sequential>& net_manager )
        : m_net_manager( net_manager ) {}

    void process( const tinymage<float>& img )
    {
        m_cropped_numbers = get_cropped_numbers( img );
        m_cropped_numbers.normalize( 0.f, 255.f );
        m_cropped_numbers.auto_threshold();
        m_cropped_numbers.display();

        std::vector<t_digit_interval> number_intervals;
        compute_ranges( m_cropped_numbers, number_intervals );

        float output[10] = { 0.f };

        std::cout << "ocr_helper::process - started inferring numbers on detected intervals" << std::endl;

        for ( auto& ni : number_intervals )
        {
            if ( ( ni.second - ni.first ) < 10 ) // letter is thinner than 10px, too small!!
            {
                std::cout << "ocr_helper::process - digit is too thin, skipping..." << std::endl;
                continue;
            }

            std::cout << "ocr_helper::process - cropping at " << ni.first << " " << ni.second << std::endl;
            tinymage<float> cropped_number( m_cropped_numbers.get_columns( ni.first, ni.second ) );

            std::cout << "ocr_helper::process - centering number" << std::endl;
            center_number( cropped_number );

            // convert imagefile to vec_t
            cropped_number.canvas_resize( 32, 32 );
            cropped_number.normalize( -1.f, 1.f );

			// recognize using data augmentation
            auto res = _compute_augmented_output( cropped_number );

            auto max_score = std::max_element( res.begin(), res.end() );
            auto max_index = static_cast<std::size_t>( std::distance( res.begin(), max_score ) );
            auto max_score_val = *max_score;

            std::cout << "ocr_helper::process - max comp idx: " << max_index << " max comp val: " << max_score_val << std::endl;

            m_recognitions.emplace_back( reco{ ni.first, max_index, 100.f * max_score_val } );

        	cropped_number.display();
        }

        std::cout << "ocr_helper::process - ended inferring numbers on detected intervals" << std::endl;
    }

    const tinymage<float>& cropped_numbers()
    {
        return m_cropped_numbers;
    }

public:

    const std::vector<ocr_helper::reco>& recognitions() { return m_recognitions; }

    std::string reco_string()
    {
        std::string _str;
        for ( auto& _reco : m_recognitions )
            _str += std::to_string( _reco.value );
        return _str;
    }

private:

    vec_t _compute_augmented_output( tinymage<float>& img )
    {
        // ONLY ROTATION AUGMENTATION IS IMPLEMENTED YET
        std::array<float,11> rotations{ -5.f, -4.f, -3.f, -2.f, -1.f, 0.f, 1.f, 2.f, 3.f, 4.f, 5.f };

        std::vector<vec_t> vec_res;

        for ( const auto& rot : rotations )
        {
            // rotate and predict
            auto rotated = img.get_rotate( rot, -1.f );
            //rotated.display();
            vec_res.emplace_back(
                m_net_manager.predict( vec_t( rotated.data(), rotated.data() + rotated.size() ) )
            );
        }

        // TODO : not very proud of the efficiency of this code section...
        // still it is temporary, as computing a mean image would also makes sense!

        auto i{0}, l{0};
        auto max = std::numeric_limits<float>::min();
        for ( const auto& res : vec_res )
        {
            // TODO : code sequance already used before, factorize somehow
            auto max_score = std::max_element( res.begin(), res.end() );
            auto max_index = static_cast<std::size_t>( std::distance( res.begin(), max_score ) );
            auto max_score_val = *max_score;

            std::cout << "network_manager::compute_augmented_output - " << max_score_val << " " << max_index << std::endl;

            if ( max_score_val > max )
            {
                l = i;
                max = max_score_val;
            }
            i++;
        }

        return vec_res.at(l);
    }

private:

    std::vector<ocr_helper::reco> m_recognitions;
    tinymage<float> m_cropped_numbers;
    network<sequential>& m_net_manager;
};

ocr_helper::ocr_helper( network<sequential>& net_manager )
    : m_pimpl( new ocr_helper_impl( net_manager ) )
{
}

ocr_helper::~ocr_helper()
{
    // https://stackoverflow.com/questions/9954518/stdunique-ptr-with-an-incomplete-type-wont-compile
}

void ocr_helper::process( const tinymage<float>& img )
{
    m_pimpl->process( img );
}

const tinymage<float>& ocr_helper::cropped_numbers()
{
    return m_pimpl->cropped_numbers();
}

const std::vector<ocr_helper::reco>& ocr_helper::recognitions()
{
    return m_pimpl->recognitions();
}

std::string ocr_helper::reco_string()
{
    return m_pimpl->reco_string();
}

//********************** UTILITY FUNCTIONS **********************//

tinymage<float> get_cropped_numbers( const tinymage<float>& input )
{
    tinymage<unsigned char> work( input.convert<unsigned char>() );

    // returns [0...255] clamped image
    tinymage<unsigned char> work_edge = work.get_sobel();

    work_edge.normalize( 0, 255 );
    //work_edge.display();

    std::cout << "ocr_helper::get_cropped_numbers - image mean value is " << static_cast<int>( work_edge.mean() ) << std::endl;
    // TODO " , noise variance is " << work_edge.variance_noise() << std::endl;

    // TODO
    // if ( work_edge.variance_noise() > 10.f )
    // {
    // 	work_edge.erode( 3 );
    // 	std::cout << "ocr_helper::get_cropped_numbers - post erosion mean value is " << work_edge.mean() << " , post erosion noise variance is " << work_edge.variance_noise() << std::endl;
    // }

    work_edge.threshold( 40 );
    //work_edge.display();

    // Compute row sums image
    tinymage<float> row_sums = work_edge.convert<float>().row_sums();
    row_sums.threshold( 5.f );
    //row_sums.display();

    // Compute line sums image
    tinymage<float> line_sums = work_edge.convert<float>().line_sums();
    line_sums.threshold( 5.f );
    //line_sums.display();

    // Compute extraction coords
    std::size_t startX = 0;
    std::size_t stopX = row_sums.width();
    tinymage_forX( row_sums, x )
    {
        if ( row_sums[x] )
        {
            startX = x;
            break;
        }
    }
    tinymage_forX( row_sums, x )
    {
        if ( row_sums[ row_sums.width() - x - 1 ] )
        {
            stopX = row_sums.width() - x - 1;
            break;
        }
    }

    std::size_t startY = 0;
    std::size_t stopY = line_sums.height();
    tinymage_forY( line_sums, y )
    {
        if ( line_sums[y] )
        {
            startY = y;
            break;
        }
    }
    tinymage_forY( line_sums, y )
    {
        if ( line_sums[ line_sums.height() - y - 1 ] )
        {
            stopY = line_sums.height() - y - 1;
            break;
        }
    }

    // apply margin & check boundaries
    std::size_t margin = ( stopY - startY ) / 7; // empirical ratio...
    startX -= std::min( startX, 2 * margin );
    startY -= std::min( startX, margin );
    stopX += std::min( input.width() - stopX - 1, 2 * margin );
    stopY += std::min( input.height() - stopY - 1, margin );

    std::cout << "ocr_helper::get_cropped_numbers - " << margin << " / " << startX << " " << startY << " " << stopX << " " << stopY << std::endl;

    return 1.f - input.get_crop( startX, startY, stopX, stopY );
}

void compute_ranges( const tinymage<float>& input, std::vector<t_digit_interval>& number_intervals )
{
    // Compute row sums image
    tinymage<float> row_sums( input.row_sums() );

    row_sums.threshold( 1.f );

    //row_sums.display();

    // Detect letter ranges
    size_t first = 0;
    bool last_val = false;
    tinymage_forX( row_sums, x )
    {
        bool cur_val = row_sums[x] > 0.f;

        if ( cur_val != last_val )
        {
            if ( !last_val ) // ascending front
            {
                first = x;
            }
            else //descending front
            {
                if ( first != 0 )
                {
                    std::cout << "compute_ranges - detected interval " << first << " " << x << std::endl;
                    number_intervals.push_back( std::make_pair( first, x ) );
                    first = 0;
                }
            }
        }
        last_val = cur_val;
    }
}

void center_number( tinymage<float>& input )
{
    // Compute row sums image
    tinymage<float> row_sums( input.row_sums() );
    row_sums.threshold( 2.f );
    //row_sums.display();

    std::size_t startX = 0;
    std::size_t stopX = row_sums.width();
    bool last_val = false;
    tinymage_forX( row_sums, x )
    {
        bool cur_val = row_sums[x] > 0.f;

        if ( cur_val != last_val )
        {
            if ( !last_val ) // ascending front
                startX = x;
            else //descending front
            {
                stopX = x;
                break;
            }
        }
        last_val = cur_val;
    }

    // Compute line sums image
    tinymage<float> line_sums( input.line_sums() );
    line_sums.threshold( 2.f );
    //line_sums.display();

    std::size_t startY = 0;
    std::size_t stopY = line_sums.height();
    last_val = false;
    tinymage_forY( line_sums, y )
    {
        bool cur_val = line_sums[y] > 0.f;

        if ( cur_val != last_val )
        {
            if ( !last_val ) // ascending front
                startY = y;
            else //descending front
            {
                stopY = y;
                break;
            }
        }
        last_val = cur_val;
    }

    std::cout << "center_number - startX/stopX - " << startX << "/" << stopX << " startY/stopY - " << startY << "/" << stopY << std::endl;

    if ( ( stopX <= startX ) || ( stopY <= startY ) )
    {
        std::cout << "center_number - invalid centering request..." << std::endl;
        return;
    }

    // try to prepare image like MNIST does:
    // http://yann.lecun.com/exdb/mnist/

    //std::size_t max_dim = std::max( input.width(), input.height() );
    std::size_t max_dim = std::max( stopX - startX, stopY - startY );

    input.crop( startX, startY, stopX, stopY );
    input.canvas_resize( max_dim, max_dim, 0.5f, 0.5f );
    input.resize( 20, 20 );
    input.normalize( 0, 255 );

    // compute center of mass
    std::size_t massX = 0;
    std::size_t massY = 0;
    std::size_t num = 0;
    tinymage_forXY( input, x, y )
    {
        massX += input.at( x, y ) * x;
        massY += input.at( x, y ) * y;
        num += input.at( x, y );
    }
    massX /= num;
    massY /= num;

    std::cout << "center_number - Mass center X=" << massX << " Y=" << massY << std::endl;

    input.canvas_resize(    28, 28,
                            1.f - static_cast<float>( massX ) / 20.f,
                            1.f - static_cast<float>( massY ) / 20.f );

    input.normalize( 0.f, 1.f );

    //input.display();
}
